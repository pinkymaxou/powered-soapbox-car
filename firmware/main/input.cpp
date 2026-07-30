// input.cpp — Gamepad input backend + MANDATORY calibration (persisted to NVS).
// The Bluetooth backend (input_bp32.c, Bluepad32/BTstack) fills the live state via the hooks
// inputbp_on_data()/inputbp_on_conn(); pairing is driven by inputbp_pair()/unpair().
//
// Calibration: capture of the center (stick at rest) then of the extremes (sticks fully deflected) → per-axis
// scale persisted. get() returns calibrated axes [-1..1]. A pairing CLEARS the calibration.
#include "input.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "config.hpp"
#include "control_math.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char* TAG = "input";

// Bluetooth backend (input_bp32.c).
extern "C" void inputbp_start(void);
extern "C" void inputbp_pair(void);
extern "C" void inputbp_unpair(void);

namespace
{
constexpr char NVS_NS[] = "pad";

// Live state (written by the BT backend, read by the control task).
std::atomic<int64_t> m_last_report_us{0};   // heartbeat: timestamp of the last HID report received
std::atomic<float> m_raw_x{0.f};
std::atomic<float> m_raw_y{0.f};
std::atomic<bool>  m_connected{false};
std::atomic<bool>  m_estop{false};
std::atomic<bool>  m_start{false};       // gamepad START/Options button (arming)
std::atomic<uint32_t> m_buttons{0};      // button mask: buttons | (misc<<16) (display)
std::atomic<float> m_zl{0.f};            // left analog trigger [0..1]
std::atomic<float> m_zr{0.f};            // right analog trigger [0..1]
std::atomic<float> m_rx2{0.f};           // right stick X [-1..1] (display)
std::atomic<float> m_ry2{0.f};           // right stick Y [-1..1] (display)
std::atomic<bool>  m_pairing{false};

// Rumble request (posted by any task, consumed by the BT thread).
std::atomic<bool>     m_rumble_pending{false};
std::atomic<unsigned> m_rumble_strong{0};
std::atomic<unsigned> m_rumble_weak{0};
std::atomic<unsigned> m_rumble_dur{0};
std::atomic<int>   m_battery{-1};
char               m_name[40] = "";

// Calibration (center + half-amplitude per axis).
std::atomic<bool>  m_calibrated{false};
std::atomic<float> m_cx{0.f};
std::atomic<float> m_cy{0.f};
std::atomic<float> m_hx{1.f};
std::atomic<float> m_hy{1.f};

// Collecting the extremes during calibration. Atomics: written by the BT task
// (inputbp_on_data) and read/initialized by the web task (calStart/calFinish).
std::atomic<int>   m_cal_state{0};   // 0 = idle, 1 = collecting
std::atomic<float> m_min_x{0.f}, m_max_x{0.f}, m_min_y{0.f}, m_max_y{0.f};

void calSave()
{
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READWRITE, &h)) return;
    const float v[4] = {m_cx.load(), m_cy.load(), m_hx.load(), m_hy.load()};
    nvs_set_blob(h, "cal", v, sizeof(v));
    nvs_set_u8(h, "ok", m_calibrated.load() ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void calLoad()
{
    nvs_handle_t h;
    if (ESP_OK != nvs_open(NVS_NS, NVS_READONLY, &h)) return;
    float v[4];
    size_t sz = sizeof(v);
    uint8_t ok = 0;
    if (ESP_OK == nvs_get_blob(h, "cal", v, &sz) && sizeof(v) == sz)
    {
        m_cx.store(v[0]); m_cy.store(v[1]); m_hx.store(v[2]); m_hy.store(v[3]);
    }
    nvs_get_u8(h, "ok", &ok);
    m_calibrated.store(0 != ok);
    nvs_close(h);
}

void calClear()
{
    m_calibrated.store(false);
    m_cal_state.store(0);
    calSave();
}
} // namespace

int64_t input::lastReportUs() { return m_last_report_us.load(); }

namespace
{
// Calibration requires FRESH DATA from the gamepad (HID report < 250 ms, same
// threshold as the heartbeat): without it, calStart would capture a center at zero (no report
// ever received) or stale values from a previous connection.
bool padDataFresh()
{
    return m_connected.load() &&
           (esp_timer_get_time() - m_last_report_us.load()) < hw::PAD_HB_TIMEOUT_US;
}
} // namespace

// ── Hooks called by the BT backend (input_bp32.c) ──
// Gamepad frame: normalized axes ~[-1..1] + buttons (emergency stop, START, display mask).
extern "C" void inputbp_on_data(float x, float y, int estop, int start, uint32_t buttons,
                                float zl, float zr, float rx2, float ry2)
{
    m_last_report_us.store(esp_timer_get_time());
    m_raw_x.store(x);
    m_raw_y.store(y);
    m_estop.store(0 != estop);
    m_start.store(0 != start);
    m_buttons.store(buttons);
    m_zl.store(zl);
    m_zr.store(zr);
    m_rx2.store(rx2);
    m_ry2.store(ry2);
    if (1 == m_cal_state.load())   // collecting the extremes (sole writer here)
    {
        if (x < m_min_x.load()) m_min_x.store(x);
        if (x > m_max_x.load()) m_max_x.store(x);
        if (y < m_min_y.load()) m_min_y.store(y);
        if (y > m_max_y.load()) m_max_y.store(y);
    }
}

// Connection state: called on (dis)connect and on every frame (fresh battery).
extern "C" void inputbp_on_conn(int connected, const char* name, int batt)
{
    if (0 == connected && 1 == m_cal_state.load())
    {
        m_cal_state.store(0);   // gamepad lost mid-collection → calibration canceled
        ESP_LOGW(TAG, "Gamepad disconnected during calibration → canceled");
    }
    m_connected.store(0 != connected);
    m_battery.store(batt);
    if (connected)
    {
        m_pairing.store(false);   // pairing done once connected
        // Called on EVERY HID frame (fresh battery): only copy the name if it changes —
        // avoids 60 useless strncpy/s and the window of reading a name being written.
        if (name && 0 != std::strcmp(m_name, name))
        {
            std::strncpy(m_name, name, sizeof(m_name) - 1);
            m_name[sizeof(m_name) - 1] = '\0';
        }
    }
    else
    {
        m_name[0] = '\0';
    }
}

void input::rumble(uint8_t strong, uint8_t weak, uint16_t dur_ms)
{
    m_rumble_strong.store(strong);
    m_rumble_weak.store(weak);
    m_rumble_dur.store(dur_ms);
    m_rumble_pending.store(true);   // posted last: the BT thread will read consistent fields
}

// Consumed by the BT thread (input_bp32.c): returns 1 and the magnitudes if a rumble is pending.
extern "C" int inputbp_take_rumble(uint8_t* strong, uint8_t* weak, uint16_t* dur)
{
    if (!m_rumble_pending.exchange(false)) return 0;
    *strong = static_cast<uint8_t>(m_rumble_strong.load());
    *weak = static_cast<uint8_t>(m_rumble_weak.load());
    *dur = static_cast<uint16_t>(m_rumble_dur.load());
    return 1;
}

void input::init()
{
    calLoad();
    inputbp_start();   // starts the BTstack/Bluepad32 task (core 0)
    ESP_LOGI(TAG, "Gamepad input: Bluepad32 backend started. Calibrated: %s",
             m_calibrated.load() ? "yes" : "no");
}

input::State input::get()
{
    input::State s;
    s.connected = m_connected.load();
    s.estop = m_estop.load();
    s.start = m_start.load();
    s.buttons = m_buttons.load();
    s.zl = m_zl.load();
    s.zr = m_zr.load();
    s.rx2 = std::clamp(m_rx2.load(), -1.f, 1.f);   // right stick (display only)
    s.ry2 = std::clamp(m_ry2.load(), -1.f, 1.f);
    // RAW stick (always provided, even uncalibrated) for the real-time display.
    s.rx = std::clamp(m_raw_x.load(), -1.f, 1.f);
    s.ry = std::clamp(m_raw_y.load(), -1.f, 1.f);
    // Calibrated axes forced to 0 while NOT calibrated — and also while COLLECTING the
    // extremes. Recalibration happens on an already-calibrated pad, and the wizard tells the
    // driver to push both sticks to their stops: if the kart were still armed, those sweeps
    // would be live full-throttle commands. Zeroing here plus calibrated()==false below (which
    // raises the blocking NOCAL fault → disarm + brake) closes both ends: starting a
    // calibration disarms the kart, and it cannot be re-armed until the wizard finishes.
    if (!m_calibrated.load() || 1 == m_cal_state.load())
    {
        return s;
    }
    const float hx = m_hx.load(), hy = m_hy.load();
    s.x = (hx > 0.05f) ? (m_raw_x.load() - m_cx.load()) / hx : 0.f;
    s.y = (hy > 0.05f) ? (m_raw_y.load() - m_cy.load()) / hy : 0.f;
    s.x = std::clamp(s.x, -1.f, 1.f);
    s.y = std::clamp(s.y, -1.f, 1.f);

    // Circle→square compensation (see control_math.hpp): makes the square's corners reachable
    // → full forward AND full steering simultaneously.
    ctl::squareMap(s.x, s.y);
    return s;
}

// Pairing / unpairing / starting a calibration are all REFUSED while the kart is armed.
// All three erase or rewrite NVS (flash write = the cache suspends and the control loop
// freezes mid-drive), pairing/unpairing yanks the very device that is steering, and the
// calibration wizard asks for full-stop stick sweeps. The web page greys the buttons out;
// this is the guard that holds when it doesn't.
namespace
{
bool refusedArmed(const char* what)
{
    if (!statusSnapshot().m_arming) return false;
    ESP_LOGW(TAG, "%s refused: kart armed — disarm first", what);
    return true;
}
} // namespace

void input::startPairing()
{
    if (refusedArmed("Pairing")) return;
    m_pairing.store(true);
    calClear();        // a new gamepad = calibration to redo
    inputbp_pair();    // opens the BT scan/pairing
    ESP_LOGI(TAG, "Pairing requested → calibration cleared");
}

void input::unpair()
{
    if (refusedArmed("Unpairing")) return;
    m_connected.store(false);
    m_pairing.store(false);
    m_name[0] = '\0';
    m_battery.store(-1);
    calClear();
    inputbp_unpair();   // disconnects + clears the BT keys
    ESP_LOGI(TAG, "Unpairing + calibration cleared");
}

void input::calStart()
{
    if (refusedArmed("Calibration")) return;   // calstate stays 0 → page stays at rest
    if (!padDataFresh())
    {
        ESP_LOGW(TAG, "Calibration refused: no recent data from the gamepad");
        return;   // calstate stays 0 → the page stays in the "rest" state
    }
    const float x = m_raw_x.load(), y = m_raw_y.load();
    m_cx.store(x); m_cy.store(y);
    m_min_x = m_max_x = x;
    m_min_y = m_max_y = y;
    m_cal_state.store(1);
    ESP_LOGI(TAG, "Calibration: center captured, move the sticks fully");
}

void input::calFinish()
{
    if (1 != m_cal_state.load()) return;
    const float cx = m_cx.load(), cy = m_cy.load();
    const float hx = std::max(cx - m_min_x, m_max_x - cx);
    const float hy = std::max(cy - m_min_y, m_max_y - cy);
    m_cal_state.store(0);
    if (!padDataFresh())
    {
        ESP_LOGW(TAG, "Calibration rejected: the gamepad stopped emitting during collection");
    }
    else if (hx > 0.2f && hy > 0.2f)   // sufficient amplitude
    {
        m_hx.store(hx); m_hy.store(hy);
        m_calibrated.store(true);
        calSave();
        ESP_LOGI(TAG, "Calibration OK (hx=%.2f hy=%.2f)", hx, hy);
    }
    else
    {
        ESP_LOGW(TAG, "Invalid calibration (amplitude too small) → ignored");
    }
}

void input::calCancel() { m_cal_state.store(0); }
int  input::calState()  { return m_cal_state.load(); }
// Reported NOT calibrated during collection, on purpose: the controller turns that into the
// blocking NOCAL fault, so a calibration started while driving disarms the kart at the next
// tick and arming stays refused until calFinish/calCancel (see the matching gate in get()).
bool input::calibrated(){ return m_calibrated.load() && 1 != m_cal_state.load(); }
bool        input::pairing() { return m_pairing.load(); }
const char* input::name()    { return m_name; }
int         input::battery() { return m_battery.load(); }

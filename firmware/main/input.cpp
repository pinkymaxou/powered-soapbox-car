// input.cpp — Backend d'entrée manette + calibration OBLIGATOIRE (persistée NVS).
// Le backend Bluetooth (input_bp32.c, Bluepad32/BTstack) remplit l'état live via les hooks
// inputbp_on_data()/inputbp_on_conn() ; l'appairage est piloté par inputbp_pair()/unpair().
//
// Calibration : capture du centre (manche au repos) puis des extrêmes (sticks à fond) → échelle
// par axe persistée. get() renvoie des axes calibrés [-1..1]. Un appairage EFFACE la calibration.
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

// Backend Bluetooth (input_bp32.c).
extern "C" void inputbp_start(void);
extern "C" void inputbp_pair(void);
extern "C" void inputbp_unpair(void);

namespace
{
constexpr char NVS_NS[] = "pad";

// État live (écrit par le backend BT, lu par la tâche de contrôle).
std::atomic<int64_t> m_last_report_us{0};   // heartbeat : date du dernier rapport HID reçu
std::atomic<float> m_raw_x{0.f};
std::atomic<float> m_raw_y{0.f};
std::atomic<bool>  m_connected{false};
std::atomic<bool>  m_estop{false};
std::atomic<bool>  m_start{false};       // bouton START/Options manette (armement)
std::atomic<uint32_t> m_buttons{0};      // masque boutons : boutons | (misc<<16) (affichage)
std::atomic<float> m_zl{0.f};            // gâchette analogique gauche [0..1]
std::atomic<float> m_zr{0.f};            // gâchette analogique droite [0..1]
std::atomic<float> m_rx2{0.f};           // stick droit X [-1..1] (affichage)
std::atomic<float> m_ry2{0.f};           // stick droit Y [-1..1] (affichage)
std::atomic<bool>  m_pairing{false};

// Requête de vibration (posée par n'importe quelle tâche, consommée par le thread BT).
std::atomic<bool>     m_rumble_pending{false};
std::atomic<unsigned> m_rumble_strong{0};
std::atomic<unsigned> m_rumble_weak{0};
std::atomic<unsigned> m_rumble_dur{0};
std::atomic<int>   m_battery{-1};
char               m_name[40] = "";

// Calibration (centre + demi-amplitude par axe).
std::atomic<bool>  m_calibrated{false};
std::atomic<float> m_cx{0.f};
std::atomic<float> m_cy{0.f};
std::atomic<float> m_hx{1.f};
std::atomic<float> m_hy{1.f};

// Collecte des extrêmes pendant la calibration. Atomiques : écrits par la tâche BT
// (inputbp_on_data) et lus/initialisés par la tâche web (calStart/calFinish).
std::atomic<int>   m_cal_state{0};   // 0 = inactif, 1 = collecte
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
// La calibration exige des DONNÉES fraîches de la manette (rapport HID < 250 ms, même
// seuil que le heartbeat) : sans ça, calStart capturerait un centre à zéro (aucun rapport
// jamais reçu) ou des valeurs périmées d'une connexion précédente.
bool padDataFresh()
{
    return m_connected.load() &&
           (esp_timer_get_time() - m_last_report_us.load()) < hw::PAD_HB_TIMEOUT_US;
}
} // namespace

// ── Hooks appelés par le backend BT (input_bp32.c) ──
// Trame manette : axes normalisés ~[-1..1] + boutons (arrêt d'urgence, START, masque affichage).
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
    if (1 == m_cal_state.load())   // collecte des extrêmes (seul écrivain ici)
    {
        if (x < m_min_x.load()) m_min_x.store(x);
        if (x > m_max_x.load()) m_max_x.store(x);
        if (y < m_min_y.load()) m_min_y.store(y);
        if (y > m_max_y.load()) m_max_y.store(y);
    }
}

// État de connexion : appelé sur (dé)connexion et à chaque trame (batterie fraîche).
extern "C" void inputbp_on_conn(int connected, const char* name, int batt)
{
    if (0 == connected && 1 == m_cal_state.load())
    {
        m_cal_state.store(0);   // manette perdue en pleine collecte → calibration annulée
        ESP_LOGW(TAG, "Manette déconnectée pendant la calibration → annulée");
    }
    m_connected.store(0 != connected);
    m_battery.store(batt);
    if (connected)
    {
        m_pairing.store(false);   // appairage terminé une fois connecté
        std::strncpy(m_name, name ? name : "", sizeof(m_name) - 1);
        m_name[sizeof(m_name) - 1] = '\0';
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
    m_rumble_pending.store(true);   // posé en dernier : le thread BT lira des champs cohérents
}

// Consommé par le thread BT (input_bp32.c) : renvoie 1 et les magnitudes si une vibration est en attente.
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
    inputbp_start();   // démarre la tâche BTstack/Bluepad32 (cœur 0)
    ESP_LOGI(TAG, "Entrée manette : backend Bluepad32 démarré. Calibré : %s",
             m_calibrated.load() ? "oui" : "non");
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
    s.rx2 = std::clamp(m_rx2.load(), -1.f, 1.f);   // stick droit (affichage seulement)
    s.ry2 = std::clamp(m_ry2.load(), -1.f, 1.f);
    // Stick BRUT (toujours fourni, même non calibré) pour l'affichage temps réel.
    s.rx = std::clamp(m_raw_x.load(), -1.f, 1.f);
    s.ry = std::clamp(m_raw_y.load(), -1.f, 1.f);
    if (!m_calibrated.load())
    {
        return s;   // axes CALIBRÉS à 0 tant que non calibré (le contrôleur refuse de rouler)
    }
    const float hx = m_hx.load(), hy = m_hy.load();
    s.x = (hx > 0.05f) ? (m_raw_x.load() - m_cx.load()) / hx : 0.f;
    s.y = (hy > 0.05f) ? (m_raw_y.load() - m_cy.load()) / hy : 0.f;
    s.x = std::clamp(s.x, -1.f, 1.f);
    s.y = std::clamp(s.y, -1.f, 1.f);

    // Compensation cercle→carré (voir control_math.hpp) : rend les coins du carré atteignables
    // → avance ET virage à fond simultanément.
    ctl::squareMap(s.x, s.y);
    return s;
}

void input::startPairing()
{
    m_pairing.store(true);
    calClear();        // une nouvelle manette = calibration à refaire
    inputbp_pair();    // ouvre le scan/appairage BT
    ESP_LOGI(TAG, "Appairage demandé → calibration effacée");
}

void input::unpair()
{
    m_connected.store(false);
    m_pairing.store(false);
    m_name[0] = '\0';
    m_battery.store(-1);
    calClear();
    inputbp_unpair();   // déconnecte + efface les clés BT
    ESP_LOGI(TAG, "Désappairage + calibration effacée");
}

void input::calStart()
{
    if (!padDataFresh())
    {
        ESP_LOGW(TAG, "Calibration refusée : aucune donnée récente de la manette");
        return;   // calstate reste 0 → la page reste à l'état « repos »
    }
    const float x = m_raw_x.load(), y = m_raw_y.load();
    m_cx.store(x); m_cy.store(y);
    m_min_x = m_max_x = x;
    m_min_y = m_max_y = y;
    m_cal_state.store(1);
    ESP_LOGI(TAG, "Calibration : centre capturé, bouger les sticks à fond");
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
        ESP_LOGW(TAG, "Calibration rejetée : la manette a cessé d'émettre pendant la collecte");
    }
    else if (hx > 0.2f && hy > 0.2f)   // amplitude suffisante
    {
        m_hx.store(hx); m_hy.store(hy);
        m_calibrated.store(true);
        calSave();
        ESP_LOGI(TAG, "Calibration OK (hx=%.2f hy=%.2f)", hx, hy);
    }
    else
    {
        ESP_LOGW(TAG, "Calibration invalide (amplitude trop faible) → ignorée");
    }
}

void input::calCancel() { m_cal_state.store(0); }
int  input::calState()  { return m_cal_state.load(); }
bool input::calibrated(){ return m_calibrated.load(); }
bool        input::pairing() { return m_pairing.load(); }
const char* input::name()    { return m_name; }
int         input::battery() { return m_battery.load(); }

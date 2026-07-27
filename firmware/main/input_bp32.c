// input_bp32.c — Bluetooth gamepad backend via Bluepad32/BTstack (a "custom" platform).
// Runs in a dedicated task (the BTstack run loop). Gamepad data is handed to the rest of the
// firmware through C hooks (implemented in input.cpp): inputbp_on_data(x, y, estop, start,
// buttons, zl, zr, rx2, ry2) and inputbp_on_conn(connected, name, batt).
// Pairing / unpairing are exposed via inputbp_pair() / inputbp_unpair().
#include <string.h>

#include <btstack.h>
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <uni.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Hooks implemented on the C++ side (input.cpp).
void inputbp_on_data(float x, float y, int estop, int start, uint32_t buttons, float zl, float zr,
                     float rx2, float ry2);
void inputbp_on_conn(int connected, const char* name, int batt);
int  inputbp_take_rumble(uint8_t* strong, uint8_t* weak, uint16_t* dur);   // rumble pending?

static uni_hid_device_t* s_dev = NULL;   // active gamepad (for a targeted disconnect)

// ── Platform callbacks ──
static void plat_init(int argc, const char** argv) { (void)argc; (void)argv; }

static void plat_on_init_complete(void)
{
    // Wi-Fi and Bluetooth share one radio (coexistence), so BT scanning only gets a slice
    // of airtime. With BTstack's default page-scan window (~11 ms listened every 1.28 s) an
    // already-paired pad switched on after boot can take several seconds to be heard. Widen
    // the incoming page-scan window so it reconnects in ~1 s. Units of 0.625 ms: interval
    // 0x0300 = 480 ms, window 0x0048 = 45 ms; page scan is INTERLACED (set in the BR/EDR
    // setup) so it listens twice per interval → ~19 % duty, a fair balance against Wi-Fi
    // throughput. Bump the window (2nd arg) toward the interval for even faster reconnect.
    gap_set_page_scan_activity(0x0300, 0x0048);

    // Reconnect an already-paired pad after a restart:
    //  - scan + autoconnect: the ESP reaches out to the known pad (link keys in NVS);
    //  - allow_incoming(true): AND we accept the pad calling us back (Switch Pro & co.
    //    re-page the host when powered on / a button is pressed).
    // Without "incoming", a paired pad could not reconnect on its own at boot.
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);
}

static uni_error_t plat_on_device_discovered(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi)
{
    (void)addr; (void)name; (void)cod; (void)rssi;
    return UNI_ERROR_SUCCESS;
}

static void plat_on_device_connected(uni_hid_device_t* d) { s_dev = d; }

static void plat_on_device_disconnected(uni_hid_device_t* d)
{
    if (d == s_dev) s_dev = NULL;
    inputbp_on_conn(0, "", -1);
}

static uni_error_t plat_on_device_ready(uni_hid_device_t* d)
{
    s_dev = d;
    inputbp_on_conn(1, d->name, -1);
    return UNI_ERROR_SUCCESS;
}

static void plat_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl)
{
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) return;
    uni_gamepad_t* gp = &ctl->gamepad;
    const float x = gp->axis_x / 512.0f;     // left stick, ~[-1..1]
    const float y = -gp->axis_y / 512.0f;    // inverted: push = forward
    const int estop = (gp->buttons & BUTTON_B) ? 1 : 0;
    const int start = (gp->misc_buttons & MISC_BUTTON_START) ? 1 : 0;
    // Display mask: buttons (bits 0-15) | misc (bits 16-19) | dpad (bits 24-27).
    const uint32_t mask = (uint32_t)gp->buttons | ((uint32_t)gp->misc_buttons << 16)
                          | ((uint32_t)gp->dpad << 24);
    const float rx2 = gp->axis_rx / 512.0f;     // RIGHT stick (display only)
    const float ry2 = -gp->axis_ry / 512.0f;    // inverted: up = +
    // ZL/ZR triggers: analog if the gamepad exposes them (brake/throttle). Some gamepads
    // ("Switch Pro" mode) report only the on/off bit → fall back to 0/100 %.
    float zl = gp->brake / 1023.0f;
    float zr = gp->throttle / 1023.0f;
    if (zl < 0.01f && (gp->buttons & BUTTON_TRIGGER_L)) zl = 1.0f;
    if (zr < 0.01f && (gp->buttons & BUTTON_TRIGGER_R)) zr = 1.0f;
    const int batt = (ctl->battery == 255) ? -1 : (ctl->battery * 100 / 254);
    inputbp_on_data(x, y, estop, start, mask, zl, zr, rx2, ry2);
    inputbp_on_conn(1, d->name, batt);

    // Haptics: if a rumble is pending, play it (BT thread = safe context).
    uint8_t rs, rw; uint16_t rd;
    if (d->report_parser.play_dual_rumble && inputbp_take_rumble(&rs, &rw, &rd))
    {
        d->report_parser.play_dual_rumble(d, 0, rd, rw, rs);
    }
}

static const uni_property_t* plat_get_property(uni_property_idx_t idx) { (void)idx; return NULL; }

static void plat_on_oob_event(uni_platform_oob_event_t event, void* data) { (void)event; (void)data; }

static struct uni_platform* get_platform(void)
{
    static struct uni_platform plat = {
        .name = "kart",
        .init = plat_init,
        .on_init_complete = plat_on_init_complete,
        .on_device_discovered = plat_on_device_discovered,
        .on_device_connected = plat_on_device_connected,
        .on_device_disconnected = plat_on_device_disconnected,
        .on_device_ready = plat_on_device_ready,
        .on_oob_event = plat_on_oob_event,
        .on_controller_data = plat_on_controller_data,
        .get_property = plat_get_property,
    };
    return &plat;
}

// ── BTstack task (blocking run loop) ──
static void bt_task(void* arg)
{
    (void)arg;
    btstack_init();
    uni_platform_set_custom(get_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();   // does not return
    vTaskDelete(NULL);
}

void inputbp_start(void)
{
    // Core 0 (network/BT), away from the control loop (core 1).
    xTaskCreatePinnedToCore(bt_task, "bt", 8192, NULL, 5, NULL, 0);
}

void inputbp_pair(void)
{
    uni_bt_allow_incoming_connections(true);
    uni_bt_start_scanning_and_autoconnect_safe();
}

void inputbp_unpair(void)
{
    if (s_dev) uni_bt_disconnect_device_safe(uni_hid_device_get_idx_for_instance(s_dev));
    uni_bt_del_keys_safe();
    uni_bt_stop_scanning_safe();
    uni_bt_allow_incoming_connections(false);
    inputbp_on_conn(0, "", -1);
}

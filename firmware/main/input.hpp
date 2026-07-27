// input.hpp — Control input (gamepad). Abstraction: the rest of the firmware does not
// depend on the backend (Bluepad32 / other). Normalized axes [-1..1], + connection state.
//
// Arcade convention: y = forward (+1 = ahead), x = steering (+1 = to the right).
// The differential mixing is done in controller_core.cpp.
#pragma once

#include <cstdint>

namespace input
{
int64_t lastReportUs();   // timestamp (µs, esp_timer) of the last HID report — gamepad heartbeat

struct State
{
    float x = 0.f;          // CALIBRATED steering [-1..1]  (+ = right)
    float y = 0.f;          // CALIBRATED forward [-1..1]  (+ = ahead)
    float rx = 0.f;         // RAW steering (uncalibrated) [-1..1] — for the display
    float ry = 0.f;         // RAW forward (uncalibrated) [-1..1] — for the display
    float zl = 0.f;         // left analog trigger (ZL) [0..1] — display
    float zr = 0.f;         // right analog trigger (ZR) [0..1] — display
    float rx2 = 0.f;        // RIGHT stick X [-1..1] — display only (uncalibrated)
    float ry2 = 0.f;        // RIGHT stick Y [-1..1] — display only (uncalibrated)
    uint32_t buttons = 0;   // mask: buttons | (misc<<16) | (dpad<<24) — display
    bool  connected = false; // gamepad paired AND connected
    bool  estop = false;     // gamepad stop button (e.g. B) — immediate brake
    bool  start = false;     // gamepad START/Options button — arming (like the physical button)
};

// Haptic feedback: makes the gamepad rumble (magnitudes 0..255, duration in ms).
// Safe to call from any task (the request is relayed to the BT thread).
void rumble(uint8_t strong, uint8_t weak, uint16_t dur_ms);

void        init();          // starts the backend (BT) — called once at boot
State       get();           // last state (CALIBRATED axes [-1..1]; {0,0} if not calibrated)
void        startPairing();  // opens a pairing window. ⚠️ CLEARS the calibration.
void        unpair();        // forgets the paired gamepad and disconnects (also clears the calibration)
bool        pairing();       // true if a pairing window is open
const char* name();          // gamepad model ("" if none)
int         battery();       // gamepad battery level 0..100, -1 if unknown

// Gamepad calibration — MANDATORY to drive (the controller refuses to run otherwise).
// Sequence: calStart() with the stick at rest (captures the center) → move the sticks fully
// in every direction (captures the extremes) → calFinish() (computes the scale, persists to NVS).
bool calibrated();   // true if a valid calibration is stored
void calStart();     // captures the center + starts collecting the extremes
void calFinish();    // validates + saves (NVS)
void calCancel();    // aborts the calibration in progress
int  calState();     // 0 = idle, 1 = collecting the extremes

} // namespace input

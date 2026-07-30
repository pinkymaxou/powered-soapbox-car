// evlog.hpp — Persistent EVENT LOG: why did the kart disarm, and when.
//
// Born from a real question on the bench: "le véhicule se désarme sans que je sache
// pourquoi". The web page shows the CURRENT fault mask, but a fault that lasts 300 ms
// (a starved gamepad heartbeat, an I2C glitch) is long gone by the time anyone looks.
// This keeps the answer: each event is {boot #, T+ms, code, detail}, appended to a
// dedicated 64 KB flash partition ("evlog") that survives reboots and power cuts.
//
// The control task only ever pushes into a RAM ring (single producer, lock-free) — it
// NEVER touches flash, in keeping with the "nothing writes flash while driving" rule.
// A low-priority task drains the ring to the partition ONLY while the kart is disarmed;
// kick() forces an immediate drain for the events that precede a power-off, so they land
// on flash during the hold-capacitor's grace.
#pragma once

#include <cstdint>

namespace evlog
{

// Event codes — mirrored for display in index.html (EV_DESC). Keep both in step.
enum class Ev : uint8_t
{
    Boot    = 1,   // data = esp_reset_reason()
    Arm     = 2,   // data = 0
    Disarm  = 3,   // data = fault mask at that tick (0 = manual / inactivity timeout)
    Fault   = 4,   // data = fault bits NEWLY RAISED while armed (rising edges only)
    IdleOff = 5,   // data = idle_off_min — self power-off after N minutes disarmed
    LvcOff  = 6,   // data = fault mask — power cut after 30 s of LVC
};

// One persisted record, 16 bytes, flash-friendly (a blank slot reads 0xFFFFFFFF).
struct Rec
{
    uint32_t head;   // 0xEB<<24 | code<<16 | boot_seq (boot counter, wraps at 65535)
    uint32_t t_ms;   // uptime at the event (wraps at ~49.7 days — irrelevant here)
    uint32_t data;   // per-code detail (see Ev)
    uint32_t chk;    // head ^ t_ms ^ data ^ 0xA5A5A5A5 — rejects torn/interrupted writes
};
static_assert(16 == sizeof(Rec), "Rec must stay 16 bytes (flash layout)");

void init();                          // find the partition, scan, log Boot — BEFORE the tasks
void push(Ev code, uint32_t data);    // control-task safe: RAM ring only, never blocks
void kick();                          // ask the drain task to flush NOW (pre-power-off)

// Read the last `cap` persisted records into `out` (oldest first); returns the count.
// `total` (optional) receives the number of records on flash; `pending` the RAM ones
// not yet drained (non-zero means "disarm and refresh to see the rest").
int  read(Rec* out, int cap, uint32_t* total = nullptr, uint32_t* pending = nullptr);
bool clear();                         // erase the partition (refused while armed)

} // namespace evlog

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
// maintain(), called from the LED task's 20 Hz loop, drains the ring to the partition ONLY
// while the kart is disarmed. No dedicated task: the first version had one, and its 3 KB
// stack helped push an already-tight heap to heap_min = 864 bytes — the page choked. The
// 50 ms cadence still lands a pre-power-off record well inside the hold-capacitor's ~1 s.
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
void maintain();                      // drain RAM → flash if disarmed; call at a few Hz (leds task)

// Chunked read API — the web reply is encoded STRAIGHT from flash in small bites, so no
// task keeps a records-buffer alive in permanent BSS (a 1.6 KB one died in the RAM audit).
// stats() snapshots the journal; readAt() copies records [idx, idx+cap) — records are
// immutable once written and only ever appended, so a snapshot stays valid across chunks.
void stats(uint32_t& total, uint32_t& pending);   // records on flash / raised-not-drained
int  readAt(uint32_t idx, Rec* out, int cap);     // valid records copied (torn ones skipped)
bool clear();                         // erase the partition (refused while armed)

} // namespace evlog

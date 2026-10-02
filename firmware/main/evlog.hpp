// evlog.hpp — Persistent EVENT LOG: why did the kart disarm, and when.
//
// Born from a real question on the bench: "le véhicule se désarme sans que je sache
// pourquoi". The web page shows the CURRENT cause, but a cause that lasts 300 ms
// (a starved gamepad heartbeat, a tap on B) is long gone by the time anyone looks.
// This keeps the answer: each event is {boot #, T+ms, code, detail}, appended to a
// dedicated 64 KB flash partition ("evlog") that survives reboots and power cuts.
//
// The control task only ever pushes into a RAM ring (single producer, lock-free) — it
// NEVER touches flash, in keeping with the "nothing writes flash while driving" rule.
// maintain(), called from the LED task's 20 Hz loop, drains the ring to the partition ONLY
// while the kart is disarmed. No dedicated task: the first version had one, and its 3 KB
// stack helped push an already-tight heap to heap_min = 864 bytes — the page choked. The
// 50 ms cadence is plenty: nothing is racing a power cut any more — the kart is switched off
// by hand (main switch or e-stop), and an unflushed record is lost, which is acceptable for
// a disarmed kart sitting idle.
#pragma once

#include <cstdint>

namespace evlog
{

// Event codes — mirrored for display in index.html (EV_DESC). Keep both in step.
enum class Ev : uint8_t
{
    Boot    = 1,   // data = esp_reset_reason()
    Arm     = 2,   // data = 0
    Disarm  = 7,   // data = the Stop cause of that tick (Stop::None = manual / inactivity)
    // RETIRED codes — a partition written by an older firmware is still on the kart, so these
    // stay reserved and keep their old meaning when the page reads the journal back:
    //   3 = the old Disarm, whose data was a fault BITMASK (fb:: bits, gone 2026-09-30) —
    //       which is exactly why the new one took a fresh code instead of quietly changing
    //       what `data` means: the same number would have decoded as a mask of causes that
    //       never happened;
    //   4 = Fault raised mid-run (there are no faults any more);
    //   5 = IdleOff and 6 = LvcOff, retired with the power latch — the firmware cannot cut
    //       its own supply.
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

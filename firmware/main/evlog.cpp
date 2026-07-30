// evlog.cpp — Persistent event log implementation (see evlog.hpp for the why).
//
// Flash layout: a flat append journal of 16-byte Rec slots in the "evlog" partition.
// A blank (erased) slot starts with 0xFFFFFFFF; a valid one with 0xEB in the top byte
// and a matching checksum. At boot we scan for the first blank slot and the highest
// boot counter; when the partition fills up it is erased and the journal restarts —
// 64 KB / 16 B = 4096 events, months of history for a log this quiet.
#include "evlog.hpp"

#include <atomic>
#include <cstring>

#include "config.hpp"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "evlog";

namespace
{
constexpr uint32_t MAGIC = 0xEBu;
constexpr uint32_t BLANK = 0xFFFFFFFFu;

// RAM ring, single producer (control task) / single consumer (maintain(), leds task).
// Powers of two only: the indices are free-running and masked. 32 pending events is far
// beyond what one drive can produce — each needs an arm/disarm/fault EDGE, not a tick.
constexpr uint32_t RING_N = 32;
struct PendingRec { uint8_t code; uint32_t t_ms; uint32_t data; };
PendingRec            m_ring[RING_N];
std::atomic<uint32_t> m_head{0};   // written by push()
std::atomic<uint32_t> m_tail{0};   // written by the drain task

const esp_partition_t* m_part = nullptr;
uint32_t          m_write_off = 0;       // next free byte offset in the partition
uint16_t          m_boot_seq = 0;        // this boot's sequence number
SemaphoreHandle_t m_mtx = nullptr;       // flash offset + read/clear vs drain

uint32_t recHead(evlog::Ev code, uint16_t boot)
{
    return (MAGIC << 24) | (static_cast<uint32_t>(code) << 16) | boot;
}

bool recValid(const evlog::Rec& r)
{
    return (r.head >> 24) == MAGIC && r.chk == (r.head ^ r.t_ms ^ r.data ^ 0xA5A5A5A5u);
}

// Append records at m_write_off (caller holds m_mtx). Erases and wraps when full —
// losing the old history to keep logging beats the reverse.
void flashAppend(const evlog::Rec* recs, int n)
{
    if (!m_part || n <= 0) return;
    const uint32_t bytes = static_cast<uint32_t>(n) * sizeof(evlog::Rec);
    if (m_write_off + bytes > m_part->size)
    {
        ESP_LOGW(TAG, "Partition full: erased, the journal restarts");
        if (ESP_OK != esp_partition_erase_range(m_part, 0, m_part->size)) return;
        m_write_off = 0;
    }
    if (ESP_OK == esp_partition_write(m_part, m_write_off, recs, bytes)) m_write_off += bytes;
}

// Drain the RAM ring to flash (caller holds m_mtx). Returns the number drained.
int drainLocked()
{
    evlog::Rec batch[16];
    int total = 0;
    for (;;)
    {
        int n = 0;
        uint32_t tail = m_tail.load(std::memory_order_relaxed);
        const uint32_t head = m_head.load(std::memory_order_acquire);
        while (tail != head && n < 16)
        {
            const PendingRec& p = m_ring[tail % RING_N];
            batch[n].head = recHead(static_cast<evlog::Ev>(p.code), m_boot_seq);
            batch[n].t_ms = p.t_ms;
            batch[n].data = p.data;
            batch[n].chk  = batch[n].head ^ batch[n].t_ms ^ batch[n].data ^ 0xA5A5A5A5u;
            ++n;
            ++tail;
        }
        if (0 == n) return total;
        flashAppend(batch, n);
        m_tail.store(tail, std::memory_order_release);
        total += n;
    }
}

} // namespace

// Drain the ring to flash, ONLY while the kart is disarmed: esp_partition_write suspends
// the flash cache, which freezes the 500 Hz control loop, and that is exactly the family
// of stall this project hunts down. Events raised while driving simply wait in RAM; the
// disarm that ends the run is itself an event, so the drain that follows carries the whole
// story out at once. Hosted by the LED task's 20 Hz loop — the empty-ring check below is
// two atomic loads, so the piggy-back costs nothing when there is nothing to do.
void evlog::maintain()
{
    if (!m_part) return;
    if (m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_relaxed)) return;
    KartStatus st;
    if (statusTrySnapshot(st) && st.m_arming) return;   // status busy? the next call will tell
    xSemaphoreTake(m_mtx, portMAX_DELAY);
    drainLocked();
    xSemaphoreGive(m_mtx);
}

void evlog::init()
{
    m_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      static_cast<esp_partition_subtype_t>(0x40), "evlog");
    if (!m_part)
    {
        // Old partition table still on flash (the table is only rewritten by `idf.py flash`).
        // The log is a diagnostic comfort: log the absence and keep booting.
        ESP_LOGW(TAG, "'evlog' partition not found: event log disabled (reflash the partition table)");
        return;
    }
    m_mtx = xSemaphoreCreateMutex();

    // Scan: first blank slot = append point; highest boot counter seen + 1 = ours.
    uint16_t last_boot = 0;
    uint32_t off = 0;
    Rec r;
    for (; off < m_part->size; off += sizeof(Rec))
    {
        if (ESP_OK != esp_partition_read(m_part, off, &r, sizeof(r))) break;
        if (BLANK == r.head) break;
        if (recValid(r))
        {
            const uint16_t b = static_cast<uint16_t>(r.head & 0xFFFF);
            if (b >= last_boot) last_boot = b;
        }
    }
    m_write_off = off;
    m_boot_seq = static_cast<uint16_t>(last_boot + 1);
    ESP_LOGI(TAG, "Event log: boot #%u, %lu records on flash",
             m_boot_seq, static_cast<unsigned long>(off / sizeof(Rec)));

    // The Boot record is written DIRECTLY: the tasks are not running yet, flash is fair game.
    Rec boot;
    boot.head = recHead(Ev::Boot, m_boot_seq);
    boot.t_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    boot.data = static_cast<uint32_t>(esp_reset_reason());
    boot.chk  = boot.head ^ boot.t_ms ^ boot.data ^ 0xA5A5A5A5u;
    flashAppend(&boot, 1);
}

void evlog::push(Ev code, uint32_t data)
{
    if (!m_part) return;
    const uint32_t head = m_head.load(std::memory_order_relaxed);
    if (head - m_tail.load(std::memory_order_acquire) >= RING_N) return;   // full: drop, never block
    PendingRec& p = m_ring[head % RING_N];
    p.code = static_cast<uint8_t>(code);
    p.t_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    p.data = data;
    m_head.store(head + 1, std::memory_order_release);
}

int evlog::read(Rec* out, int cap, uint32_t* total, uint32_t* pending)
{
    if (pending) *pending = m_head.load() - m_tail.load();
    if (!m_part || cap <= 0)
    {
        if (total) *total = 0;
        return 0;
    }
    xSemaphoreTake(m_mtx, portMAX_DELAY);
    const int on_flash = static_cast<int>(m_write_off / sizeof(Rec));
    if (total) *total = static_cast<uint32_t>(on_flash);
    const int n = (on_flash < cap) ? on_flash : cap;
    const uint32_t first = m_write_off - static_cast<uint32_t>(n) * sizeof(Rec);
    int got = 0;
    for (int i = 0; i < n; ++i)
    {
        Rec r;
        if (ESP_OK != esp_partition_read(m_part, first + i * sizeof(Rec), &r, sizeof(r))) break;
        if (recValid(r)) out[got++] = r;   // a torn record is skipped, not fatal
    }
    xSemaphoreGive(m_mtx);
    return got;
}

bool evlog::clear()
{
    if (!m_part) return false;
    KartStatus st;
    if (statusTrySnapshot(st) && st.m_arming) return false;   // same rule as every flash write
    xSemaphoreTake(m_mtx, portMAX_DELAY);
    const bool ok = (ESP_OK == esp_partition_erase_range(m_part, 0, m_part->size));
    if (ok)
    {
        m_write_off = 0;
        // Re-anchor the journal so the NEXT event does not float in an empty file.
        Rec boot;
        boot.head = recHead(Ev::Boot, m_boot_seq);
        boot.t_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        boot.data = static_cast<uint32_t>(esp_reset_reason());
        boot.chk  = boot.head ^ boot.t_ms ^ boot.data ^ 0xA5A5A5A5u;
        flashAppend(&boot, 1);
    }
    xSemaphoreGive(m_mtx);
    return ok;
}

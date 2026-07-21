// ringbuffer.hpp — Circular buffer (header-only), reusable, generic element type.
// Keeps the last CAP values; copyTo() linearizes them (from oldest to most
// recent) — e.g. for the protobuf "bytes" encoding of the history (T = uint8_t).
#pragma once

#include <cstdint>

template <typename T, int CAP>
class Ring
{
public:
    void push(T v)
    {
        m_buf[m_head] = v;
        m_head = (m_head + 1) % CAP;
        if (m_count < CAP)
        {
            ++m_count;
        }
    }

    int count() const { return m_count; }
    int capacity() const { return CAP; }

    // Linearized copy (from oldest to most recent) into dst; returns the number of bytes.
    // For the protobuf "bytes" encoding (1 byte/sample, zero heap).
    int copyTo(T* dst, int cap) const
    {
        const int n = (m_count < cap) ? m_count : cap;
        const int oldest = (m_head - m_count + CAP) % CAP;
        for (int i = 0; i < n; ++i) dst[i] = m_buf[(oldest + i) % CAP];
        return n;
    }

private:
    T   m_buf[CAP] = {};
    int     m_count = 0;
    int     m_head = 0;
};

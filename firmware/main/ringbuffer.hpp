// ringbuffer.hpp — Buffer circulaire d'octets (header-only), réutilisable.
// Conserve les CAP dernières valeurs et sait les sérialiser en tableau JSON.
#pragma once

#include <cstdint>
#include <string>

template <int CAP>
class Ring
{
public:
    void push(uint8_t v)
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

    // Ajoute  "name":[v0,v1,...]  à `out`, du plus ancien au plus récent.
    void appendJson(std::string& out, const char* name) const
    {
        out += '"';
        out += name;
        out += "\":[";
        int oldest = (m_head - m_count + CAP) % CAP;
        for (int i = 0; i < m_count; ++i)
        {
            if (i)
            {
                out += ',';
            }
            out += std::to_string(m_buf[(oldest + i) % CAP]);
        }
        out += ']';
    }

private:
    uint8_t m_buf[CAP] = {};
    int     m_count = 0;
    int     m_head = 0;
};

// ringbuffer.hpp — Buffer circulaire d'octets (header-only), réutilisable.
// Conserve les CAP dernières valeurs et sait les sérialiser en tableau JSON.
#pragma once

#include <cstdio>
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
        char buf[16];
        out += '"';
        out += name;
        out += "\":[";
        const int oldest = (m_head - m_count + CAP) % CAP;
        for (int i = 0; i < m_count; ++i)
        {
            if (i)
            {
                out += ',';
            }
            snprintf(buf, sizeof(buf), "%u", m_buf[(oldest + i) % CAP]);
            out += buf;
        }
        out += ']';
    }

    // Variante SANS TAS :  "name":[v0,...]  écrit dans dst (borné, terminé par NUL).
    // Retourne le nombre d'octets écrits (hors NUL) ; s'arrête proprement si dst est plein.
    size_t appendJsonC(char* dst, size_t cap, const char* name) const
    {
        size_t n = 0;
        auto put = [&](const char* s) {
            while (*s && n + 1 < cap) dst[n++] = *s++;
        };
        put("\""); put(name); put("\":[");
        const int oldest = (m_head - m_count + CAP) % CAP;
        char num[16];
        for (int i = 0; i < m_count; ++i)
        {
            if (i) put(",");
            snprintf(num, sizeof(num), "%u", m_buf[(oldest + i) % CAP]);
            put(num);
        }
        put("]");
        dst[n] = '\0';
        return n;
    }

private:
    uint8_t m_buf[CAP] = {};
    int     m_count = 0;
    int     m_head = 0;
};

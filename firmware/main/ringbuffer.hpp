// ringbuffer.hpp — Buffer circulaire (header-only), réutilisable, type d'élément générique.
// Conserve les CAP dernières valeurs ; copyTo() les linéarise (du plus ancien au plus
// récent) — p. ex. pour l'encodage protobuf « bytes » de l'historique (T = uint8_t).
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

    // Copie linéarisée (du plus ancien au plus récent) dans dst ; retourne le nb d'octets.
    // Pour l'encodage protobuf « bytes » (1 octet/échantillon, zéro tas).
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

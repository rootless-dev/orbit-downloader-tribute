#include "torrent/Bitfield.h"

Bitfield::Bitfield(int pieceCount)
    : m_pieceCount(pieceCount), m_bits(pieceCount > 0 ? pieceCount : 0, 0) {}

int Bitfield::size() const { return m_pieceCount; }

bool Bitfield::has(int i) const {
    if (i < 0 || i >= m_pieceCount) return false;
    return m_bits[i] != 0;
}

void Bitfield::set(int i) {
    if (i < 0 || i >= m_pieceCount) return;
    m_bits[i] = 1;
}

int Bitfield::count() const {
    int n = 0;
    for (uint8_t b : m_bits) n += (b != 0);
    return n;
}

bool Bitfield::isComplete() const {
    return m_pieceCount > 0 && count() == m_pieceCount;
}

QByteArray Bitfield::toBytes() const {
    const int byteCount = (m_pieceCount + 7) / 8;
    QByteArray out(byteCount, char(0));
    for (int i = 0; i < m_pieceCount; ++i) {
        if (m_bits[i]) {
            const int byte = i / 8;
            const int bit = 7 - (i % 8);
            out[byte] = char(uint8_t(out[byte]) | (1u << bit));
        }
    }
    return out;
}

Bitfield Bitfield::fromBytes(const QByteArray& b, int pieceCount) {
    Bitfield bf(pieceCount);
    for (int i = 0; i < pieceCount; ++i) {
        const int byte = i / 8;
        const int bit = 7 - (i % 8);
        if (byte >= b.size()) continue;
        if ((uint8_t(b[byte]) >> bit) & 1u) bf.set(i);
    }
    return bf;
}

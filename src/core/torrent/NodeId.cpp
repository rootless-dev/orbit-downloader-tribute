#include "torrent/NodeId.h"
#include <QRandomGenerator>
#include <random>

NodeId NodeId::fromSeed(quint32 seed) {
    std::mt19937 rng(seed);
    QByteArray b(20, '\x00');
    for (int i = 0; i < 20; ++i) b[i] = char(rng() & 0xFF);
    return NodeId(b);
}

NodeId NodeId::random() {
    QByteArray b(20, '\x00');
    QRandomGenerator::global()->generate(b.begin(), b.end());
    return NodeId(b);
}

bool NodeId::closer(const NodeId& a, const NodeId& b, const NodeId& target) {
    const QByteArray &A = a.m_bytes, &B = b.m_bytes, &T = target.m_bytes;
    for (int i = 0; i < 20; ++i) {
        quint8 da = quint8(A[i]) ^ quint8(T[i]);
        quint8 db = quint8(B[i]) ^ quint8(T[i]);
        if (da != db) return da < db;
    }
    return false;
}

int NodeId::bucketIndex(const NodeId& other) const {
    for (int i = 0; i < 20; ++i) {
        quint8 x = quint8(m_bytes[i]) ^ quint8(other.m_bytes[i]);
        if (x) { for (int bit = 7; bit >= 0; --bit) if (x & (1u << bit)) return i * 8 + (7 - bit); }
    }
    return 160;
}

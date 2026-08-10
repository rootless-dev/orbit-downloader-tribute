#pragma once
#include <QByteArray>
#include <QtGlobal>

class NodeId {
public:
    NodeId() : m_bytes(20, '\x00') {}
    explicit NodeId(const QByteArray& raw20) : m_bytes(raw20) { Q_ASSERT(raw20.size() == 20); m_bytes.resize(20); }
    static NodeId fromSeed(quint32 seed);
    static NodeId random();
    const QByteArray& bytes() const { return m_bytes; }
    bool operator==(const NodeId& o) const { return m_bytes == o.m_bytes; }
    static bool closer(const NodeId& a, const NodeId& b, const NodeId& target);
    int bucketIndex(const NodeId& other) const;
private:
    QByteArray m_bytes;
};

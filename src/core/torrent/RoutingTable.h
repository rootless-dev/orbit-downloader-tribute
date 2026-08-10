#pragma once
#include "torrent/NodeId.h"
#include <QHash>
#include <QString>
#include <QVector>

struct DhtNodeEntry { NodeId id; QString host; quint16 port = 0; };

class RoutingTable {
public:
    explicit RoutingTable(const NodeId& self, int k = 8);
    bool sawNode(const DhtNodeEntry& n);
    void markBad(const NodeId& id);
    QVector<DhtNodeEntry> closest(const NodeId& target, int count) const;
    int nodeCount() const;
    QVector<DhtNodeEntry> allNodes() const;
private:
    struct Node { DhtNodeEntry e; bool bad = false; };
    NodeId m_self; int m_k;
    QHash<int, QVector<Node>> m_buckets; // key = self.bucketIndex(node) in [0,159]
};

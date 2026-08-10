#include "torrent/RoutingTable.h"
#include <algorithm>

RoutingTable::RoutingTable(const NodeId& self, int k) : m_self(self), m_k(k) {}

bool RoutingTable::sawNode(const DhtNodeEntry& n) {
    if (n.id == m_self) return false;
    const int b = m_self.bucketIndex(n.id); // [0,159]
    QVector<Node>& bucket = m_buckets[b];
    for (Node& nd : bucket)
        if (nd.e.id == n.id) { nd.e = n; nd.bad = false; return true; } // refresh
    if (bucket.size() < m_k) { bucket.append({n, false}); return true; }
    for (Node& nd : bucket)
        if (nd.bad) { nd = {n, false}; return true; }                  // evict a bad node
    return false;                                                       // full of good nodes -> drop
}
void RoutingTable::markBad(const NodeId& id) {
    auto it = m_buckets.find(m_self.bucketIndex(id));
    if (it == m_buckets.end()) return;
    for (Node& nd : *it) if (nd.e.id == id) nd.bad = true;
}
QVector<DhtNodeEntry> RoutingTable::closest(const NodeId& target, int count) const {
    QVector<DhtNodeEntry> all = allNodes();
    std::sort(all.begin(), all.end(), [&](const DhtNodeEntry& a, const DhtNodeEntry& b) {
        return NodeId::closer(a.id, b.id, target);
    });
    if (all.size() > count) all.resize(count);
    return all;
}
int RoutingTable::nodeCount() const {
    int n = 0; for (const QVector<Node>& b : m_buckets) for (const Node& nd : b) if (!nd.bad) ++n;
    return n;
}
QVector<DhtNodeEntry> RoutingTable::allNodes() const {
    QVector<DhtNodeEntry> out;
    for (const QVector<Node>& b : m_buckets) for (const Node& nd : b) if (!nd.bad) out.append(nd.e);
    return out;
}

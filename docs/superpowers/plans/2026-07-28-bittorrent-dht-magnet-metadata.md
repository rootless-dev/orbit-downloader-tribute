# BitTorrent Sub-phase C — DHT + magnet + metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add DHT peer discovery (BEP 5), magnet links, and metadata exchange (BEP 10 + BEP 9) so torrents whose peers live on the DHT (e.g. the Ubuntu ISO) and magnet-only torrents download to completion.

**Architecture:** A single app-wide `DhtNode` (owned by `DownloadManager`, one `QUdpSocket`) is a full "good-citizen" Kademlia node that serves every torrent's `get_peers` lookups, feeding peers into the same `m_pendingPeers` path the tracker already uses. Magnet links parse to an info_hash, fetch the info dict from peers via the extension protocol + `ut_metadata`, verify it against the info_hash, then hand off to the normal `TorrentTask` download engine. New pure units (`MagnetUri`, `NodeId`, `RoutingTable`, `KrpcCodec`) are unit-tested in isolation; the networked pieces are tested with an in-process mesh of real `DhtNode`s and a test metadata peer, all offline.

**Tech Stack:** C++20, Qt 6.11 (Core + Network: `QUdpSocket`, `QHostInfo`, `QCryptographicHash`), CMake, QtTest. Reuses existing `Bencode`, `TorrentMetainfo`, `PeerConnection`, `TorrentTask`, `AnnounceController`, `PieceStore`.

## Global Constraints

- **C++20**; `orbitcore` links only Qt Core + Network (no QtWidgets) — stays headless-testable.
- **No commits by the implementer.** Project rule (carlos reviews and commits each task himself). Every task ends with the relevant suite green; do **not** run `git commit`.
- **Determinism in tests:** no wall clock, no `Math.random`/`QRandomGenerator` in test paths. Node id and transaction ids derive from an injected `rngSeed` (mirror `UdpTrackerClient`). Time is injectable.
- **Test seams (env vars), production-default-off:** `ORBIT_ALLOW_LOOPBACK_PEERS` (keep loopback nodes/peers), `ORBIT_UDP_FAST_TIMEOUT` (short timeouts). Both already exist — reuse, do not invent new ones.
- **Gate suites must stay intact:** `tst_download`, `tst_ftp`, `tst_torrent` keep passing with unchanged expectations.
- **Build:** configure `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`; build a target `cmake --build build --target <t> -j4`; run `./build/tests/<t>` or `ctest --test-dir build --output-on-failure`.
- **Deferred to Sub-phase D (do NOT implement here):** `announce_peer` of our own torrents, serving `ut_metadata`/pieces to others, PEX/`ut_pex`, accepting incoming BT connections. DHT-IPv6 (BEP 32), BEP 42, uTP/encryption are out.

---

## File structure

New files under `src/core/torrent/`:

- `MagnetUri.{h,cpp}` — pure magnet parser.
- `NodeId.{h,cpp}` — 160-bit id + XOR distance.
- `RoutingTable.{h,cpp}` — Kademlia k-buckets.
- `KrpcCodec.{h,cpp}` — KRPC encode/decode over `Bencode`.
- `DhtNode.{h,cpp}` — the networked node (client + server + bootstrap + persistence).
- `MetadataFetch.{h,cpp}` — magnet metadata orchestrator.

Modified:

- `src/core/torrent/PeerWire.{h,cpp}` — extension-protocol / ut_metadata framing helpers.
- `src/core/torrent/PeerConnection.{h,cpp}` — extension protocol + metadata mode.
- `src/core/torrent/TorrentTask.{h,cpp}` — DHT peer source + `FetchingMetadata`.
- `src/core/torrent/TorrentMetainfo.{h,cpp}` — build metainfo from a raw info dict.
- `src/core/DownloadManager.{h,cpp}` — own `DhtNode`; `addMagnet`; wire DHT to tasks.
- `src/core/DownloadState.h` — add `FetchingMetadata`.
- GUI: `src/gui/MainWindow.*`, `src/gui/TorrentOpenDialog.*`, `src/gui/NewDownloadDialog.*`, clipboard watcher, `DownloadTableModel`, Preferences BitTorrent page, `SettingsIo`.

New tests under `tests/`:

- `tst_magneturi.cpp`, `tst_nodeid.cpp`, `tst_routingtable.cpp`, `tst_krpc.cpp`, `tst_dht.cpp`, `tst_utmetadata.cpp`, `tst_metadatafetch.cpp`, plus additions to `tst_peerwire.cpp`, `tst_torrent.cpp`, `tst_settings.cpp`, `tst_gui.cpp`.
- `tests/TestMetadataPeer.{h,cpp}` — in-process peer that serves the info dict via ut_metadata.

Each new `tst_*` needs an `add_executable` + `add_test` pair in `tests/CMakeLists.txt` (fold into the task that introduces it).

---

## Task 1: `MagnetUri` — pure magnet parser

**Files:**
- Create: `src/core/torrent/MagnetUri.h`, `src/core/torrent/MagnetUri.cpp`
- Test: `tests/tst_magneturi.cpp`
- Modify: `tests/CMakeLists.txt`, `src/core/CMakeLists.txt` (add `torrent/MagnetUri.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  struct MagnetInfo {
      QByteArray infoHash;          // 20 raw bytes, or empty if parse failed
      QString    displayName;       // dn (may be empty)
      QStringList trackers;         // all tr= values, in order
      bool isValid() const { return infoHash.size() == 20; }
  };
  namespace MagnetUri { MagnetInfo parse(const QString& uri); }
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tst_magneturi.cpp
#include <QtTest>
#include "torrent/MagnetUri.h"

class TstMagnetUri : public QObject {
    Q_OBJECT
private slots:
    void parsesHexInfoHashNameAndTrackers() {
        const QString uri = "magnet:?xt=urn:btih:143b885127dfa398b9c58f4abc7f3145b91f5f4f"
                            "&dn=ubuntu-24.04.4-desktop-arm64.iso"
                            "&tr=https%3A%2F%2Ftorrent.ubuntu.com%2Fannounce"
                            "&tr=https%3A%2F%2Fipv6.torrent.ubuntu.com%2Fannounce";
        const MagnetInfo m = MagnetUri::parse(uri);
        QVERIFY(m.isValid());
        QCOMPARE(m.infoHash.toHex(), QByteArray("143b885127dfa398b9c58f4abc7f3145b91f5f4f"));
        QCOMPARE(m.displayName, QString("ubuntu-24.04.4-desktop-arm64.iso"));
        QCOMPARE(m.trackers.size(), 2);
        QCOMPARE(m.trackers.at(0), QString("https://torrent.ubuntu.com/announce"));
    }
    void parsesBase32InfoHash() {
        // base32 of the same 20 bytes (32 chars, RFC 4648, uppercase)
        const QString uri = "magnet:?xt=urn:btih:CQ5YQUJH36RZRONFR5FLY7ZRIW4R6X2P";
        const MagnetInfo m = MagnetUri::parse(uri);
        QVERIFY(m.isValid());
        QCOMPARE(m.infoHash.toHex(), QByteArray("143b885127dfa398b9c58f4abc7f3145b91f5f4f"));
    }
    void rejectsNonMagnetOrMissingXt() {
        QVERIFY(!MagnetUri::parse("https://example.com").isValid());
        QVERIFY(!MagnetUri::parse("magnet:?dn=foo").isValid());
        QVERIFY(!MagnetUri::parse("magnet:?xt=urn:btih:zzzz").isValid()); // bad hash
    }
};
QTEST_APPLESS_MAIN(TstMagnetUri)
#include "tst_magneturi.moc"
```

- [ ] **Step 2: Add build wiring, run test, verify it fails to compile/link**

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(tst_magneturi tst_magneturi.cpp)
target_link_libraries(tst_magneturi PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_magneturi COMMAND tst_magneturi)
```
Add `torrent/MagnetUri.cpp` to the `orbitcore` sources list in `src/core/CMakeLists.txt`.
Run: `cmake --build build --target tst_magneturi -j4`
Expected: FAIL — `MagnetUri.h` not found.

- [ ] **Step 3: Implement**

```cpp
// src/core/torrent/MagnetUri.h
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

struct MagnetInfo {
    QByteArray infoHash;
    QString    displayName;
    QStringList trackers;
    bool isValid() const { return infoHash.size() == 20; }
};

namespace MagnetUri { MagnetInfo parse(const QString& uri); }
```
```cpp
// src/core/torrent/MagnetUri.cpp
#include "torrent/MagnetUri.h"
#include <QUrl>
#include <QUrlQuery>

namespace {
// RFC 4648 base32 decode (uppercase A-Z 2-7). Returns empty on invalid input.
QByteArray base32Decode(const QString& s) {
    static const QString A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QByteArray out; int buffer = 0, bits = 0;
    for (QChar c : s.toUpper()) {
        int v = A.indexOf(c);
        if (v < 0) return {};
        buffer = (buffer << 5) | v; bits += 5;
        if (bits >= 8) { bits -= 8; out.append(char((buffer >> bits) & 0xFF)); }
    }
    return out;
}
QByteArray decodeInfoHash(const QString& xt) {
    // xt = "urn:btih:<hex40|base32-32>"
    const QString prefix = "urn:btih:";
    if (!xt.startsWith(prefix)) return {};
    const QString h = xt.mid(prefix.size());
    if (h.size() == 40) {
        const QByteArray raw = QByteArray::fromHex(h.toLatin1());
        return raw.size() == 20 ? raw : QByteArray();
    }
    if (h.size() == 32) {
        const QByteArray raw = base32Decode(h);
        return raw.size() == 20 ? raw : QByteArray();
    }
    return {};
}
} // namespace

MagnetInfo MagnetUri::parse(const QString& uri) {
    MagnetInfo m;
    if (!uri.startsWith("magnet:?")) return m;
    const QUrlQuery q(uri.mid(QString("magnet:?").size()));
    for (const auto& kv : q.queryItems(QUrl::FullyDecoded)) {
        if (kv.first == "xt" && m.infoHash.isEmpty()) m.infoHash = decodeInfoHash(kv.second);
        else if (kv.first == "dn") m.displayName = kv.second;
        else if (kv.first == "tr") m.trackers.append(kv.second);
    }
    return m;
}
```

- [ ] **Step 4: Run tests, verify pass**

Run: `cmake --build build --target tst_magneturi -j4 && ./build/tests/tst_magneturi`
Expected: PASS (3 cases). Do NOT commit.

---

## Task 2: `NodeId` + XOR distance — pure

**Files:**
- Create: `src/core/torrent/NodeId.h`, `src/core/torrent/NodeId.cpp`
- Test: `tests/tst_nodeid.cpp`
- Modify: `tests/CMakeLists.txt`, `src/core/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  class NodeId {                               // 160-bit, big-endian, 20 bytes
  public:
      NodeId() = default;                       // all-zero
      explicit NodeId(const QByteArray& raw20); // exactly 20 bytes
      static NodeId fromSeed(quint32 seed);     // deterministic (tests)
      static NodeId random();                   // production
      const QByteArray& bytes() const;
      bool operator==(const NodeId&) const;
      // XOR distance compare: is |a^target| < |b^target| ?
      static bool closer(const NodeId& a, const NodeId& b, const NodeId& target);
      int bucketIndex(const NodeId& other) const; // index of highest differing bit [0..159], 160 if equal
  };
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tst_nodeid.cpp
#include <QtTest>
#include "torrent/NodeId.h"

class TstNodeId : public QObject {
    Q_OBJECT
private slots:
    void closerByXorDistance() {
        NodeId target(QByteArray(20, '\x00'));
        NodeId a(QByteArray(19, '\x00') + QByteArray(1, '\x01')); // distance 1
        NodeId b(QByteArray(19, '\x00') + QByteArray(1, '\xFF')); // distance 255
        QVERIFY(NodeId::closer(a, b, target));
        QVERIFY(!NodeId::closer(b, a, target));
    }
    void bucketIndexIsHighestDifferingBit() {
        NodeId zero(QByteArray(20, '\x00'));
        NodeId one(QByteArray(19, '\x00') + QByteArray(1, '\x01')); // differ in last bit
        QCOMPARE(zero.bucketIndex(one), 159);
        QCOMPARE(zero.bucketIndex(zero), 160);
        NodeId top(QByteArray(1, '\x80') + QByteArray(19, '\x00'));
        QCOMPARE(zero.bucketIndex(top), 0);
    }
    void fromSeedIsDeterministic() {
        QCOMPARE(NodeId::fromSeed(42).bytes(), NodeId::fromSeed(42).bytes());
        QVERIFY(NodeId::fromSeed(1).bytes() != NodeId::fromSeed(2).bytes());
        QCOMPARE(NodeId::fromSeed(1).bytes().size(), 20);
    }
};
QTEST_APPLESS_MAIN(TstNodeId)
#include "tst_nodeid.moc"
```

- [ ] **Step 2: Build wiring + run, verify fails**

Add `tst_nodeid` executable/test to `tests/CMakeLists.txt` (link `orbitcore Qt6::Test`) and `torrent/NodeId.cpp` to `orbitcore`.
Run: `cmake --build build --target tst_nodeid -j4` → FAIL (no `NodeId.h`).

- [ ] **Step 3: Implement**

```cpp
// src/core/torrent/NodeId.h
#pragma once
#include <QByteArray>
class NodeId {
public:
    NodeId() : m_bytes(20, '\x00') {}
    explicit NodeId(const QByteArray& raw20) : m_bytes(raw20) { m_bytes.resize(20); }
    static NodeId fromSeed(quint32 seed);
    static NodeId random();
    const QByteArray& bytes() const { return m_bytes; }
    bool operator==(const NodeId& o) const { return m_bytes == o.m_bytes; }
    static bool closer(const NodeId& a, const NodeId& b, const NodeId& target);
    int bucketIndex(const NodeId& other) const;
private:
    QByteArray m_bytes;
};
```
```cpp
// src/core/torrent/NodeId.cpp
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
```

- [ ] **Step 4: Run tests, verify pass**

Run: `cmake --build build --target tst_nodeid -j4 && ./build/tests/tst_nodeid` → PASS. Do NOT commit.

---

## Task 3: `RoutingTable` — pure k-buckets

**Files:**
- Create: `src/core/torrent/RoutingTable.h`, `src/core/torrent/RoutingTable.cpp`
- Test: `tests/tst_routingtable.cpp`
- Modify: `tests/CMakeLists.txt`, `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `NodeId` (Task 2), `PeerAddress` (existing, from `HttpTrackerClient.h`).
- Produces:
  ```cpp
  struct DhtNodeEntry { NodeId id; QString host; quint16 port; };
  class RoutingTable {
  public:
      explicit RoutingTable(const NodeId& self, int k = 8);
      // Insert/refresh a node seen alive. Returns false if dropped (its per-bit
      // bucket is full of good nodes, no bad node to evict).
      bool sawNode(const DhtNodeEntry& n);
      void markBad(const NodeId& id);
      // The k nodes closest to target, nearest first.
      QVector<DhtNodeEntry> closest(const NodeId& target, int count) const;
      int nodeCount() const;
      QVector<DhtNodeEntry> allNodes() const; // for persistence
  };
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tst_routingtable.cpp
#include <QtTest>
#include "torrent/RoutingTable.h"

static DhtNodeEntry mk(quint32 seed, quint16 port) {
    return DhtNodeEntry{ NodeId::fromSeed(seed), "127.0.0.1", port };
}
class TstRoutingTable : public QObject {
    Q_OBJECT
private slots:
    void storesAndReturnsClosest() {
        RoutingTable rt(NodeId::fromSeed(0), 8);
        // Some inserts may be dropped: ~half of random ids land in bucket 0
        // (first bit differs), which caps at k=8. That is correct behavior, so
        // do NOT assert every sawNode() succeeds.
        for (quint32 s = 1; s <= 20; ++s) rt.sawNode(mk(s, quint16(1000 + s)));
        QVERIFY(rt.nodeCount() > 0);
        auto near = rt.closest(NodeId::fromSeed(1), 4);
        QVERIFY(near.size() >= 1 && near.size() <= 4);
        // seed-1 is inserted first (its bucket is empty then), so it is always
        // retained and is the unique nearest node to its own id.
        QCOMPARE(near.first().id.bytes(), NodeId::fromSeed(1).bytes());
    }
    void retainsNodesAcrossManyBuckets() {
        RoutingTable rt(NodeId::fromSeed(0), 2); // small k per bucket
        int stored = 0;
        for (quint32 s = 1; s <= 40; ++s) if (rt.sawNode(mk(s, quint16(s)))) ++stored;
        // Random ids spread across many per-bit buckets, each capped at k=2,
        // so the table retains far more than a single bucket's worth.
        QVERIFY(stored > 2);
    }
    void markBadFreesSpaceInSameBucket() {
        // self = all-zero; two ids whose highest differing bit is identical
        // (last byte 0x02 and 0x03 -> bucketIndex 158 for both) share a bucket.
        RoutingTable rt(NodeId(QByteArray(20, '\x00')), 1); // k=1
        DhtNodeEntry a{ NodeId(QByteArray(19,'\x00') + QByteArray(1,'\x02')), "127.0.0.1", 1 };
        DhtNodeEntry b{ NodeId(QByteArray(19,'\x00') + QByteArray(1,'\x03')), "127.0.0.1", 2 };
        QVERIFY(rt.sawNode(a));
        QVERIFY(!rt.sawNode(b));   // bucket full (k=1), a is good -> b dropped
        rt.markBad(a.id);
        QVERIFY(rt.sawNode(b));    // a marked bad -> evicted, b admitted
    }
};
QTEST_APPLESS_MAIN(TstRoutingTable)
#include "tst_routingtable.moc"
```

- [ ] **Step 2: Build wiring + run, verify fails**

Add `tst_routingtable` to CMake, `torrent/RoutingTable.cpp` to `orbitcore`. Run target → FAIL.

- [ ] **Step 3: Implement**

**Bucket model:** one bucket per bit-position `[0..159]`, keyed by
`self.bucketIndex(node)` (the standard fixed-per-bit form of a Kademlia table —
equivalent to always splitting the self-bucket, but simpler and with no fragile
range-split recursion). Each bucket holds up to `k` nodes.

```cpp
// src/core/torrent/RoutingTable.h
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
```
```cpp
// src/core/torrent/RoutingTable.cpp
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
```

- [ ] **Step 4: Run tests, verify pass**

Run: `cmake --build build --target tst_routingtable -j4 && ./build/tests/tst_routingtable` → PASS. Do NOT commit.

---

## Task 4: `KrpcCodec` — KRPC over Bencode (+ robustness)

**Files:**
- Create: `src/core/torrent/KrpcCodec.h`, `src/core/torrent/KrpcCodec.cpp`
- Test: `tests/tst_krpc.cpp`
- Modify: `tests/CMakeLists.txt`, `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `Bencode` (`src/core/torrent/Bencode.h`) — `BencodeValue`, `Bencode::encode/decode`.
- Produces:
  ```cpp
  struct KrpcMessage {
      enum Kind { Query, Response, Error, Invalid };
      Kind kind = Invalid;
      QByteArray tid;                 // "t"
      QString method;                 // query name ("ping"/"find_node"/"get_peers"/"announce_peer")
      QMap<QByteArray, BencodeValue> args; // "a" (query) or "r" (response)
      int errorCode = 0; QString errorMsg;
  };
  namespace KrpcCodec {
      QByteArray encodeQuery(const QByteArray& tid, const QString& method,
                             const QMap<QByteArray, BencodeValue>& args);
      QByteArray encodeResponse(const QByteArray& tid, const QMap<QByteArray, BencodeValue>& r);
      QByteArray encodeError(const QByteArray& tid, int code, const QString& msg);
      KrpcMessage decode(const QByteArray& datagram); // kind==Invalid on any malformed input
  }
  ```
  (If `Bencode` does not already expose an ordered dict builder returning `BencodeValue`, use whatever constructor `Bencode.h` provides — check `tst_bencode.cpp` for the exact API and mirror it.)

- [ ] **Step 1: Write the failing test** (includes a malformed-input robustness case)

```cpp
// tests/tst_krpc.cpp
#include <QtTest>
#include "torrent/KrpcCodec.h"

class TstKrpc : public QObject {
    Q_OBJECT
private slots:
    void roundTripsPingQuery() {
        QMap<QByteArray, BencodeValue> a; a["id"] = BencodeValue(QByteArray(20, '\x01'));
        QByteArray dg = KrpcCodec::encodeQuery("aa", "ping", a);
        KrpcMessage m = KrpcCodec::decode(dg);
        QCOMPARE(m.kind, KrpcMessage::Query);
        QCOMPARE(m.tid, QByteArray("aa"));
        QCOMPARE(m.method, QString("ping"));
        QCOMPARE(m.args["id"].toByteArray(), QByteArray(20, '\x01'));
    }
    void roundTripsResponseAndError() {
        QMap<QByteArray, BencodeValue> r; r["id"] = BencodeValue(QByteArray(20, '\x02'));
        QCOMPARE(KrpcCodec::decode(KrpcCodec::encodeResponse("bb", r)).kind, KrpcMessage::Response);
        KrpcMessage e = KrpcCodec::decode(KrpcCodec::encodeError("cc", 201, "Generic"));
        QCOMPARE(e.kind, KrpcMessage::Error);
        QCOMPARE(e.errorCode, 201);
    }
    void garbageIsInvalidNeverCrashes() {
        for (const QByteArray& bad : { QByteArray(), QByteArray("x"), QByteArray("d"),
                                       QByteArray("d1:td2:te"), QByteArray(2000, '\xFF') })
            QCOMPARE(KrpcCodec::decode(bad).kind, KrpcMessage::Invalid);
    }
};
QTEST_APPLESS_MAIN(TstKrpc)
#include "tst_krpc.moc"
```

- [ ] **Step 2: Build wiring + run, verify fails.** Add `tst_krpc` + `torrent/KrpcCodec.cpp`. Run → FAIL.

- [ ] **Step 3: Implement** using `Bencode`. Build a top-level dict: `t`, `y` (`q`/`r`/`e`), and `q`+`a` for queries, `r` for responses, `e` list `[code, msg]` for errors. `decode` wraps `Bencode::decode` and returns `Invalid` whenever decoding throws/fails or the required keys are absent/wrong-typed. (Mirror the ordered-dict construction used in `TorrentMetainfo.cpp`/`tst_bencode.cpp`.)

- [ ] **Step 4: Run tests, verify pass.** `./build/tests/tst_krpc` → PASS (incl. garbage case). Do NOT commit.

---

## Task 5: `DhtNode` transport + `ping`

**Files:**
- Create: `src/core/torrent/DhtNode.h`, `src/core/torrent/DhtNode.cpp`
- Test: `tests/tst_dht.cpp`
- Modify: `tests/CMakeLists.txt`, `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `NodeId`, `RoutingTable`, `KrpcCodec`, `PeerAddress`.
- Produces:
  ```cpp
  class DhtNode : public QObject {
      Q_OBJECT
  public:
      DhtNode(const NodeId& self, quint16 port, quint32 rngSeed, QObject* parent = nullptr);
      bool start();                 // bind UDP socket; false on bind failure
      quint16 boundPort() const;
      int nodeCount() const;        // routing table size (diagnostics)
      void ping(const QString& host, quint16 port);
      void lookup(const QByteArray& infoHash);          // Task 8
      const NodeId& id() const;
  signals:
      void peersFound(QByteArray infoHash, QVector<PeerAddress> peers); // Task 8
      void bootstrapped();                                              // Task 9
  private:
      // transaction map, socket readyRead dispatch, query/response handling
  };
  ```
  Transaction ids: 2 bytes, incremented from `rngSeed`. Timeouts use a base of 15s, shortened when `ORBIT_UDP_FAST_TIMEOUT` is set.

- [ ] **Step 1: Write the failing test** (two real nodes ping each other)

```cpp
// tests/tst_dht.cpp
#include <QtTest>
#include "torrent/DhtNode.h"

class TstDht : public QObject {
    Q_OBJECT
private:
    void initTestCase() { qputenv("ORBIT_ALLOW_LOOPBACK_PEERS", "1");
                          qputenv("ORBIT_UDP_FAST_TIMEOUT", "1"); }
private slots:
    void pingPopulatesRoutingTable() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2);
        QVERIFY(a.start()); QVERIFY(b.start());
        a.ping("127.0.0.1", b.boundPort());
        QTRY_VERIFY_WITH_TIMEOUT(a.nodeCount() >= 1, 3000); // learned b from its pong
        QTRY_VERIFY_WITH_TIMEOUT(b.nodeCount() >= 1, 3000); // learned a from its ping
    }
};
QTEST_MAIN(TstDht)
#include "tst_dht.moc"
```
(Note `initTestCase` must be a private slot; shown above for brevity — place it under `private slots:`.)

- [ ] **Step 2: Build wiring + run, verify fails.** Add `tst_dht` (link `orbitcore Qt6::Test`, `QTEST_MAIN` needs the Qt event loop) + `torrent/DhtNode.cpp`. Run → FAIL.

- [ ] **Step 3: Implement** the socket plumbing: bind `QUdpSocket` on `port` (0 = ephemeral), `readyRead` → read datagrams → `KrpcCodec::decode`. On an incoming `ping` query, insert the sender into the routing table and reply with a `response` carrying our `id`. On a `response` matching a pending transaction, insert the responder into the routing table. Maintain `QHash<QByteArray, Pending>` keyed by tid, each with a `QTimer` (15s, or from `ORBIT_UDP_FAST_TIMEOUT`) that on fire **drops the pending entry**. (Marking a node *bad* on timeout belongs to the maintenance/health-check re-ping path — Task 9 — where the target `NodeId` is known; a first-contact `ping(host,port)` has no id to mark, so it only drops the transaction.) Insert the sender of *any* valid message into the table (`sawNode`).

- [ ] **Step 4: Run tests, verify pass.** `./build/tests/tst_dht` → PASS. Do NOT commit.

---

## Task 6: `DhtNode` `find_node` (client + server)

**Files:** Modify `src/core/torrent/DhtNode.{h,cpp}`; Test `tests/tst_dht.cpp`.

**Interfaces:**
- Produces: internal `void findNode(const QString& host, quint16 port, const NodeId& target)`; incoming `find_node` queries answered with the `nodes` compact string (26 bytes each: 20 id + 4 IPv4 + 2 port) of the `closest(target, 8)`.

- [ ] **Step 1: Write the failing test** (A learns C through B)

```cpp
    void findNodeLearnsThirdNode() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2), c(NodeId::fromSeed(3), 0, 3);
        QVERIFY(a.start()); QVERIFY(b.start()); QVERIFY(c.start());
        b.ping("127.0.0.1", c.boundPort());                 // B knows C
        QTRY_VERIFY_WITH_TIMEOUT(b.nodeCount() >= 1, 3000);
        a.findNodeForTest("127.0.0.1", b.boundPort(), c.id()); // test-only shim -> internal findNode
        QTRY_VERIFY_WITH_TIMEOUT(a.nodeCount() >= 2, 4000);   // A learned B and (via B's nodes) C
    }
```
Add a thin `void findNodeForTest(const QString&, quint16, const NodeId&)` public method that forwards to the private `findNode`, guarded for tests.

- [ ] **Step 2: Run, verify fails** (method missing). 

- [ ] **Step 3: Implement.** Encode compact `nodes` (26 bytes/entry) in `find_node`/`get_peers` responses via a helper `QByteArray packNodes(const QVector<DhtNodeEntry>&)`. On a `find_node` response, parse `nodes` and `sawNode` each. Add `unpackNodes(const QByteArray&) -> QVector<DhtNodeEntry>`.

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 7: `DhtNode` `get_peers` + token + peer store + `announce_peer` (server)

**Files:** Modify `src/core/torrent/DhtNode.{h,cpp}`; Test `tests/tst_dht.cpp`.

**Interfaces:**
- Produces: server-side peer store `QHash<QByteArray /*infoHash*/, QVector<StoredPeer>>` with expiry; token issuance `QByteArray makeToken(const QHostAddress&)` (HMAC-ish: `SHA1(secret + ip)`, secret rotated on a timer), and `bool validToken(const QByteArray&, const QHostAddress&)` (accept current + previous secret). Incoming `get_peers`: if we have stored peers for the info_hash return `values` (compact 6-byte peers) + `token`; else return `nodes` (closest) + `token`. Incoming `announce_peer`: validate `token`, then store `{sender ip, port arg}` for the info_hash.

- [ ] **Step 1: Write the failing test** (announce to B, then get_peers from A finds it)

```cpp
    void getPeersReturnsAnnouncedPeer() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2);
        QVERIFY(a.start()); QVERIFY(b.start());
        const QByteArray ih(20, '\x07');
        // C announces itself on B via the raw wire (test shim): get_peers to obtain token, then announce_peer
        b.storePeerForTest(ih, "203.0.113.9", 6881);        // test-only: seed B's peer store directly
        QVector<PeerAddress> got;
        connect(&a, &DhtNode::peersFound, this, [&](QByteArray, QVector<PeerAddress> p){ got = p; });
        a.getPeersForTest("127.0.0.1", b.boundPort(), ih);   // single-hop get_peers -> emits peersFound
        QTRY_VERIFY_WITH_TIMEOUT(!got.isEmpty(), 3000);
        QCOMPARE(got.first().host, QString("203.0.113.9"));
    }
    void announcePeerRequiresValidToken() {
        DhtNode b(NodeId::fromSeed(2), 0, 2); QVERIFY(b.start());
        QVERIFY(!b.acceptAnnounceForTest(QByteArray(20,'\x08'), "1.2.3.4", 5, "badtoken"));
    }
```
Add public test shims: `storePeerForTest`, `getPeersForTest` (single-hop, emits `peersFound`), `acceptAnnounceForTest` (returns whether token validated + stored).

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement** token secret + rotation `QTimer` (10 min; previous secret kept one rotation), `makeToken`/`validToken`, the peer store with a per-entry expiry (e.g. 30 min, swept on access), `packPeers`/`unpackPeers` (6 bytes each), and the three query handlers. `get_peers` client parses `values` → `peersFound`, else parses `nodes` for the iterative lookup (Task 8).

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 8: `DhtNode::lookup` — iterative get_peers over a mesh

**Files:** Modify `src/core/torrent/DhtNode.{h,cpp}`; Test `tests/tst_dht.cpp`.

**Interfaces:**
- Produces: public `void lookup(const QByteArray& infoHash)` running an iterative α=3 closest-node search seeded from `RoutingTable::closest(target, 8)`, querying `get_peers`, folding returned `nodes` into the shortlist, and emitting `peersFound(infoHash, peers)` for every batch of `values` seen. Terminates when the closest-unqueried set stops improving or a cap (e.g. 100 nodes queried) is hit.

- [ ] **Step 1: Write the failing test** (multi-node mesh, anti-flake loop)

```cpp
    void lookupFindsPeerAcrossMesh() {
        // router R knows everyone; nodes N1..N4 form the reachable set; holder H stores the info_hash peer.
        DhtNode R(NodeId::fromSeed(100), 0, 100); QVERIFY(R.start());
        QList<DhtNode*> mesh;
        for (quint32 s = 1; s <= 4; ++s) { auto* n = new DhtNode(NodeId::fromSeed(s), 0, s, this);
            QVERIFY(n->start()); n->ping("127.0.0.1", R.boundPort()); mesh << n; }
        DhtNode H(NodeId::fromSeed(200), 0, 200); QVERIFY(H.start());
        H.ping("127.0.0.1", R.boundPort());
        const QByteArray ih(20, '\x42');
        H.storePeerForTest(ih, "198.51.100.7", 51413);
        QTRY_VERIFY_WITH_TIMEOUT(R.nodeCount() >= 5, 4000); // R learned the mesh + H
        DhtNode seeker(NodeId::fromSeed(9), 0, 9); QVERIFY(seeker.start());
        seeker.ping("127.0.0.1", R.boundPort());
        QTRY_VERIFY_WITH_TIMEOUT(seeker.nodeCount() >= 1, 3000);
        QVector<PeerAddress> got;
        connect(&seeker, &DhtNode::peersFound, this, [&](QByteArray, QVector<PeerAddress> p){ got += p; });
        seeker.lookup(ih);
        QTRY_VERIFY_WITH_TIMEOUT(!got.isEmpty(), 8000);
        QCOMPARE(got.first().host, QString("198.51.100.7"));
    }
```
Run this test in a 10x loop locally to confirm no flake before moving on.

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement** the iterative lookup: a per-lookup struct holding the target, a sorted shortlist of `{DhtNodeEntry, queried, responded}`, and α in-flight. Seed from `closest`. Each `get_peers` response: `sawNode(responder)`, add `unpackNodes(nodes)` to the shortlist (dedup by id), emit any `values` as `peersFound`. Keep querying the α closest unqueried until none closer than the best `k` responded, or the query cap is hit.

- [ ] **Step 4: Run tests (loop), verify pass.** Do NOT commit.

---

## Task 9: `DhtNode` bootstrap (DNS) + maintenance

**Files:** Modify `src/core/torrent/DhtNode.{h,cpp}`; Test `tests/tst_dht.cpp`.

**Interfaces:**
- Produces: `void bootstrap(const QStringList& routers)` — DNS-resolves each `host:port` via `QHostInfo::lookupHost`, pings resolved addresses, then `findNode(self.id())` to fill buckets; emits `bootstrapped()` once the table has any node. Default routers live in `DhtNode` as a static list. A maintenance `QTimer` (every ~5 min, or fast under `ORBIT_UDP_FAST_TIMEOUT`) pings questionable nodes — and when such a health-check ping (which targets a *known* `NodeId`) times out, calls `RoutingTable::markBad(id)` — and refreshes stale buckets via `findNode(randomIdInBucket)`. (This is where the timeout→`markBad` behavior lives, deferred from Task 5's first-contact ping.)

- [ ] **Step 1: Write the failing test** (bootstrap against a loopback "router" node, no real DNS)

```cpp
    void bootstrapsAgainstLoopbackRouter() {
        DhtNode router(NodeId::fromSeed(100), 0, 100); QVERIFY(router.start());
        DhtNode n(NodeId::fromSeed(1), 0, 1); QVERIFY(n.start());
        QSignalSpy spy(&n, &DhtNode::bootstrapped);
        n.bootstrap({ QString("127.0.0.1:%1").arg(router.boundPort()) }); // numeric host -> no DNS needed
        QVERIFY(spy.wait(4000));
        QVERIFY(n.nodeCount() >= 1);
    }
```

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement.** Parse each router `host:port`; if host is a numeric IP, use it directly; else `QHostInfo::lookupHost` (async) and ping each resolved address. After first response, `findNode(self)` and emit `bootstrapped()`. Add the maintenance timer.

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 10: `DhtNode` persistence (`dht.dat`)

**Files:** Modify `src/core/torrent/DhtNode.{h,cpp}`; Test `tests/tst_dht.cpp`. Reuse `Persistence` (`src/core/Persistence.h`) if it fits; else a small `QDataStream` blob.

**Interfaces:**
- Produces: `void saveState(const QString& path) const` (node id + up to N good nodes), `bool loadState(const QString& path)` (restores node id + seeds the routing table). Constructor variant or a `static NodeId loadOrCreateId(const QString& path)` so the persistent node id survives restarts.

- [ ] **Step 1: Write the failing test**

```cpp
    void persistsNodeIdAndNodes() {
        QTemporaryDir dir; const QString path = dir.path() + "/dht.dat";
        {
            DhtNode a(NodeId::fromSeed(5), 0, 5); QVERIFY(a.start());
            a.seedNodeForTest(DhtNodeEntry{ NodeId::fromSeed(6), "127.0.0.1", 6881 });
            a.saveState(path);
        }
        DhtNode b(NodeId::fromSeed(999), 0, 999); QVERIFY(b.start());
        QVERIFY(b.loadState(path));
        QCOMPARE(b.id().bytes(), NodeId::fromSeed(5).bytes()); // id restored, not the ctor's
        QVERIFY(b.nodeCount() >= 1);                            // node restored
    }
```
`seedNodeForTest` = a public shim calling `RoutingTable::sawNode`.

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement** `saveState`/`loadState` with `QDataStream` (magic + version + node id + `[id,host,port]*`). `loadState` overwrites the in-memory node id and seeds the table.

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 11: Extension protocol in `PeerConnection` (BEP 10 handshake)

**Files:** Modify `src/core/torrent/PeerWire.{h,cpp}`, `src/core/torrent/PeerConnection.{h,cpp}`; Test `tests/tst_peerwire.cpp`.

**Interfaces:**
- Produces on `PeerConnection`:
  - Reserved bytes now set bit `0x10` in byte 5 (extension protocol) and `0x01` in byte 7 (DHT). New `PeerWire::handshake` gains these bits (keep the existing signature; set the bits internally).
  - A "metadata mode" constructor: `PeerConnection(const PeerAddress&, const QByteArray& infoHash, const QByteArray& peerId, RateLimiter*, QObject*)` (no `pieceCount`) — connects, sends BT handshake + the BEP 10 extended handshake, and does no bitfield/piece logic.
  - Sends its extended handshake (id 20, ext id 0) with `m = { ut_metadata: 1 }` right after the BT handshake.
  - Parses the peer's extended handshake; emits `void extendedHandshake(int utMetadataId, int metadataSize)` (utMetadataId = peer's id for ut_metadata, 0 if unsupported; metadataSize from the `metadata_size` key, 0 if absent).
  - `PeerWire`: `QByteArray extendedHandshakeMsg(int utMetadataId)`, and a parser `bool parseExtendedHandshake(const QByteArray& payload, int* utMetadataId, int* metadataSize)`.

- [ ] **Step 1: Write the failing test** (a raw server sends an extended handshake; PeerConnection reports it)

```cpp
    void parsesExtendedHandshake() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray ih(20, '\x01');
        PeerAddress addr; addr.host = "127.0.0.1"; addr.port = server.serverPort();
        PeerConnection conn(addr, ih, QByteArray(20,'\x02'), nullptr); // metadata-mode ctor
        QSignalSpy newConn(&server, &QTcpServer::newConnection);
        QSignalSpy extSpy(&conn, &PeerConnection::extendedHandshake);
        conn.connectToPeer();
        QVERIFY(newConn.count() > 0 || newConn.wait(2000));
        QTcpSocket* s = server.nextPendingConnection(); QVERIFY(s);
        while (s->bytesAvailable() < 68) QVERIFY(s->waitForReadyRead(2000));
        s->read(68);
        // reply: our handshake (ext bit set) + extended handshake advertising ut_metadata=3, metadata_size=1234
        QByteArray reply = PeerWire::handshake(ih, QByteArray(20,'\x03'));
        // d1:md11:ut_metadatai3ee13:metadata_sizei1234ee  (bencoded), framed as ext msg id 0
        QByteArray ext = "d1:md11:ut_metadatai3ee13:metadata_sizei1234ee";
        QByteArray msg; quint32 len = 2 + ext.size();
        msg.append(char((len>>24)&0xFF)); msg.append(char((len>>16)&0xFF));
        msg.append(char((len>>8)&0xFF));  msg.append(char(len&0xFF));
        msg.append(char(20)); msg.append(char(0)); msg.append(ext);
        reply += msg; s->write(reply); s->flush();
        QVERIFY(extSpy.count() > 0 || extSpy.wait(2000));
        QCOMPARE(extSpy.at(0).at(0).toInt(), 3);      // peer's ut_metadata id
        QCOMPARE(extSpy.at(0).at(1).toInt(), 1234);   // metadata_size
    }
```

- [ ] **Step 2: Run, verify fails** (metadata-mode ctor + signal missing).

- [ ] **Step 3: Implement.** Add `PeerWire::Extended = 20`. Set reserved bits in `handshake`. In `PeerConnection`, add the metadata-mode ctor (sets `m_metadataMode = true`, `m_pieceCount = 0`), send our extended handshake in `onConnected`/after BT handshake ok, and in `handleMessage` handle id 20: if ext-id byte == 0, parse the bencoded `m`/`metadata_size` and emit `extendedHandshake`. Route other ext ids to Task 12's ut_metadata handler.

- [ ] **Step 4: Run tests, verify pass.** Also run full `tst_peerwire` + `tst_torrent` to prove the reserved-bit change didn't regress normal peers. Do NOT commit.

---

## Task 12: `ut_metadata` (BEP 9) fetch + verify

**Files:** Modify `src/core/torrent/PeerConnection.{h,cpp}`, `src/core/torrent/PeerWire.{h,cpp}`; Create `tests/TestMetadataPeer.{h,cpp}`; Test `tests/tst_utmetadata.cpp`; Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Produces on `PeerConnection` (metadata mode): after `extendedHandshake` with a valid `utMetadataId`+`metadataSize`, request all pieces (`ceil(size/16384)`) via ext messages `{msg_type:0, piece:N}`; accumulate `data` messages (`{msg_type:1,...}` + trailing raw bytes); on completion emit `void metadataComplete(QByteArray infoDict)` (raw bytes, **already verified** SHA-1 == infoHash) or `void metadataFailed()` on hash mismatch/reject. `PeerWire`: `QByteArray utMetadataRequest(int extId, int piece)`.
- `TestMetadataPeer`: constructor `TestMetadataPeer(const QByteArray& infoHash, const QByteArray& infoDict)`, `quint16 port()`, serves the extension handshake (`ut_metadata` id, `metadata_size`) and answers requests with `data` pieces.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tst_utmetadata.cpp — infoDict from a known .torrent, infoHash = SHA1(infoDict)
    void fetchesAndVerifiesMetadata() {
        const QByteArray infoDict = makeInfoDict();               // helper: a valid bencoded info dict
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        TestMetadataPeer peer(ih, infoDict);
        PeerConnection conn({"127.0.0.1", peer.port()}, ih, QByteArray(20,'\x02'), nullptr);
        QSignalSpy done(&conn, &PeerConnection::metadataComplete);
        conn.connectToPeer();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.at(0).at(0).toByteArray(), infoDict);       // exact bytes recovered
    }
    void rejectsTamperedMetadata() {
        QByteArray infoDict = makeInfoDict();
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        QByteArray tampered = infoDict; tampered[10] = tampered[10] ^ 0xFF;
        TestMetadataPeer peer(ih, tampered);                      // serves wrong bytes for a right hash
        PeerConnection conn({"127.0.0.1", peer.port()}, ih, QByteArray(20,'\x02'), nullptr);
        QSignalSpy fail(&conn, &PeerConnection::metadataFailed);
        conn.connectToPeer();
        QVERIFY(fail.wait(5000));
    }
```

- [ ] **Step 2: Build wiring + run, verify fails.** Add `TestMetadataPeer.cpp` to `tst_utmetadata` sources; add the test target.

- [ ] **Step 3: Implement** the request loop + accumulation + `SHA1(assembled) == infoHash` check. `data` messages are `<bencode dict>` immediately followed by the raw 16 KiB (or short last) block — split at the end of the bencoded dict. On `reject` or mismatch, emit `metadataFailed`.

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 13: `MetadataFetch` orchestrator

**Files:** Create `src/core/torrent/MetadataFetch.{h,cpp}`; Test `tests/tst_metadatafetch.cpp`; Modify CMake. Add `TorrentMetainfo::fromInfoDict(const QByteArray& infoDict, const QStringList& trackers)` in `TorrentMetainfo.{h,cpp}`.

**Interfaces:**
- Consumes: `PeerConnection` (metadata mode, Task 12), `DhtNode::lookup`/`peersFound` (Task 8), `MagnetInfo` (Task 1), `TorrentMetainfo`.
- Produces:
  ```cpp
  class MetadataFetch : public QObject {
      Q_OBJECT
  public:
      MetadataFetch(const MagnetInfo&, DhtNode*, QNetworkAccessManager*, quint32 rngSeed,
                    RateLimiter*, QObject* parent = nullptr);
      void start();  // kick DHT lookup + magnet-tracker announces; connect to peers
      void addPeerForTest(const PeerAddress&);
  signals:
      void metainfoReady(TorrentMetainfo);
      void failed(QString reason);
  };
  ```
  `TorrentMetainfo::fromInfoDict` builds the full metainfo (name, pieceLength, pieceHashes, files, totalLength, **infoHash = SHA1(infoDict)**) from the raw info dict + trackers.

- [ ] **Step 1: Write the failing test** (peer injected; no DHT needed)

```cpp
    void producesMetainfoFromPeer() {
        const QByteArray infoDict = makeInfoDict();
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        TestMetadataPeer peer(ih, infoDict);
        MagnetInfo mi; mi.infoHash = ih; mi.displayName = "x";
        QNetworkAccessManager nam; RateLimiter rl;
        MetadataFetch mf(mi, nullptr, &nam, 1, &rl);
        QSignalSpy ready(&mf, &MetadataFetch::metainfoReady);
        mf.addPeerForTest({"127.0.0.1", peer.port()});
        mf.start();
        QVERIFY(ready.wait(5000));
        auto meta = ready.at(0).at(0).value<TorrentMetainfo>();
        QCOMPARE(meta.infoHash, ih);
        QVERIFY(meta.pieceHashes.size() > 0);
    }
```

- [ ] **Step 2: Build wiring + run, verify fails.** Ensure `TorrentMetainfo` is a registered metatype (`qRegisterMetaType<TorrentMetainfo>()`), add `tst_metadatafetch` with `TestMetadataPeer.cpp`.

- [ ] **Step 3: Implement.** On `start`, subscribe to `DhtNode::peersFound` (if non-null) for `mi.infoHash` and start `dht->lookup(mi.infoHash)`; also announce to `mi.trackers` via a reused `AnnounceController` to gather peers. For each peer, open a metadata-mode `PeerConnection`; on the first `metadataComplete`, build `TorrentMetainfo::fromInfoDict` and emit `metainfoReady`; tear down the rest. `failed` if no peer yields metadata within a cap.

- [ ] **Step 4: Run tests, verify pass.** Do NOT commit.

---

## Task 14: DHT as a peer source for `TorrentTask` (unblocks Ubuntu offline)

**Files:** Modify `src/core/DownloadManager.{h,cpp}` (own `DhtNode`, gate on settings), `src/core/torrent/TorrentTask.{h,cpp}` (subscribe to DHT). Test: `tests/tst_torrent.cpp`.

**Interfaces:**
- Consumes: `DhtNode` (Tasks 5-10).
- Produces: `TorrentTask` gains `void setDht(DhtNode*)`; when set and running, it calls `dht->lookup(m_meta.infoHash)`, connects `DhtNode::peersFound` (filtered to its own info_hash) to append into `m_pendingPeers` + `openPeers()`, and re-lookups on the same starvation trigger as the adaptive reannounce. `DownloadManager` creates one `DhtNode` when `dht.enabled` and calls `t->setDht(m_dht)` in `makeTorrentTask`.

- [ ] **Step 1: Write the failing test** (peers ONLY via an in-process DHT, then download from TestSeeder)

```cpp
    void downloadsWithPeersFromDht() {
        const QByteArray data = makeData(50000);
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384);
        // in-process DHT: holder H stores the seeder as the peer for infoHash; task's DhtNode bootstraps to H
        DhtNode holder(NodeId::fromSeed(200), 0, 200); QVERIFY(holder.start());
        holder.storePeerForTest(m.infoHash, "127.0.0.1", seeder.port());
        DhtNode dht(NodeId::fromSeed(1), 0, 1); QVERIFY(dht.start());
        dht.bootstrap({QString("127.0.0.1:%1").arg(holder.boundPort())});
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.setDht(&dht);                                   // NO addPeerForTest — peers must come from DHT
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 15000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
    }
```
(Set `ORBIT_ALLOW_LOOPBACK_PEERS`/`ORBIT_UDP_FAST_TIMEOUT` in `initTestCase`.)

- [ ] **Step 2: Run, verify fails** (`setDht` missing).

- [ ] **Step 3: Implement** `TorrentTask::setDht` + subscription (filter `peersFound` by info_hash, dedup against known peers, append + `openPeers`), re-lookup on starvation. Wire `DownloadManager` to create+own the `DhtNode` (persistent id via `dht.dat`, bootstrap default routers) and pass it in.

- [ ] **Step 4: Run tests (loop), verify pass.** Do NOT commit.

---

## Task 15: `addMagnet` + `FetchingMetadata` state + magnet resume (crown E2E)

**Files:** Modify `src/core/DownloadManager.{h,cpp}`, `src/core/DownloadState.h`, `src/core/torrent/TorrentTask.{h,cpp}` (or a small `MagnetEntry` in the manager), session persistence (`torrents.json` writer/reader). Test: `tests/tst_torrent.cpp`.

**Interfaces:**
- Produces: `QUuid DownloadManager::addMagnet(const QString& uri, const QString& destDir, PieceStrategy, const QVector<int>& selectedFiles = {})`. It parses the magnet, shows a `FetchingMetadata` entry, runs `MetadataFetch`, and on `metainfoReady` writes `torrents/<infohash>.torrent`, constructs the normal `TorrentTask` (all files unless a selection is given), and starts it. Unresolved magnets persist to `torrents.json` (`{magnet:true, infoHash, trackers, displayName, destDir}`) and are re-fetched on load. Add `DownloadState::FetchingMetadata`.

- [ ] **Step 1: Write the failing test** (full magnet → download, offline)

```cpp
    void magnetResolvesViaDhtThenDownloads() {
        const QByteArray data = makeData(50000);
        auto m = singleMeta(data, 16384);                 // gives us infoDict + infoHash + .torrent bytes
        TestSeeder seeder(m.infoHash, data, 16384);
        TestMetadataPeer metaPeer(m.infoHash, m.infoDict); // serves metadata
        // DHT holder advertises BOTH the metadata peer and the seeder for infoHash
        DhtNode holder(NodeId::fromSeed(200), 0, 200); QVERIFY(holder.start());
        holder.storePeerForTest(m.infoHash, "127.0.0.1", metaPeer.port());
        holder.storePeerForTest(m.infoHash, "127.0.0.1", seeder.port());
        DhtNode dht(NodeId::fromSeed(1), 0, 1); QVERIFY(dht.start());
        dht.bootstrap({QString("127.0.0.1:%1").arg(holder.boundPort())});
        QTemporaryDir dir; QVERIFY(dir.isValid());
        DownloadManager mgr(/* config with dht.enabled, resumeDir=dir */);
        mgr.setDhtForTest(&dht);
        const QString magnet = "magnet:?xt=urn:btih:" + m.infoHash.toHex();
        QUuid id = mgr.addMagnet(magnet, dir.path(), PieceStrategy::RarestFirst);
        QTRY_VERIFY_WITH_TIMEOUT(mgr.taskById(id) && mgr.taskById(id)->state() == DownloadState::Completed, 20000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
    }
```
(`singleMeta` must expose `.infoDict`; add it if missing.)

- [ ] **Step 2: Run, verify fails** (`addMagnet` missing).

- [ ] **Step 3: Implement** `addMagnet`, the `FetchingMetadata` transient entry (a lightweight task-like object exposing id/state/name/size so the model can render it), the handoff to `makeTorrentTask`, the `.torrent` cache write, and magnet persistence/restore (all-files default on restore). 

- [ ] **Step 4: Run tests (loop), verify pass.** Also run `tst_torrent` full + gates. Do NOT commit.

---

## Task 16: GUI — magnet entry + `FetchingMetadata` rendering

**Files:** Modify `src/gui/MainWindow.{h,cpp}` (File > Open Magnet…, drag&drop of magnet text), `src/gui/NewDownloadDialog.*` (accept `magnet:`), the clipboard watcher (detect `magnet:`), `src/gui/DownloadTableModel.*` (render `FetchingMetadata`, size "—"), post-resolve file-selection via existing `TorrentOpenDialog`. Test: `tests/tst_gui.cpp`.

**Interfaces:**
- Consumes: `DownloadManager::addMagnet`, `MagnetUri::parse`.
- Produces: a `MainWindow::openMagnet(const QString& uri)` slot; model shows `FetchingMetadata` as "Resolving magnet…" with `—` size.

- [ ] **Step 1: Write the failing test** (logic-level, headless)

```cpp
    void tableRendersFetchingMetadata() {
        DownloadTableModel model;
        // inject a row in FetchingMetadata state (via the same path addMagnet uses)
        model.upsertRow(/* id */ QUuid::createUuid(), "ubuntu.iso", DownloadState::FetchingMetadata, /*total*/ -1, /*recv*/ 0);
        const QModelIndex sizeIdx = model.index(0, DownloadTableModel::ColSize);
        QCOMPARE(model.data(sizeIdx).toString(), QString("—"));
        const QModelIndex stIdx = model.index(0, DownloadTableModel::ColStatus);
        QVERIFY(model.data(stIdx).toString().contains("metadata", Qt::CaseInsensitive));
    }
```
(Match the model's actual API — mirror an existing `tst_gui` model test for the exact method names/column enum.)

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement** the menu action, clipboard/drag detection of `magnet:` (route to `openMagnet`), `NewDownloadDialog` accepting a magnet URI, and the model rendering for `FetchingMetadata` (status text + `—` size). After `metainfoReady`, if added interactively, open `TorrentOpenDialog` for file selection before starting (or default all if the user opted "download all").

- [ ] **Step 4: Run `tst_gui`, verify pass.** Build `orbit-gui`, confirm it links + launches headless. Do NOT commit.

---

## Task 17: Preferences + settings (`dht` block)

**Files:** Modify `SettingsIo` (GUI settings), the Preferences BitTorrent page, `DownloadManager` config plumbing. Test: `tests/tst_settings.cpp`.

**Interfaces:**
- Produces: `settings.json` gains `"dht": { "enabled": true, "port": 6881 }`, round-tripped with tolerant defaults (unknown keys preserved; missing block → defaults). The Preferences BitTorrent page gains a DHT enable checkbox, a UDP port field, and a read-only DHT node-count display (from `DhtNode::nodeCount`). Toggling DHT off stops/starts the `DhtNode` live.

- [ ] **Step 1: Write the failing test**

```cpp
    void dhtBlockRoundTripsWithDefaults() {
        SettingsIo io;
        QJsonObject root; // no dht block
        auto s = io.fromJson(root);
        QCOMPARE(s.dht.enabled, true);     // default on
        QCOMPARE(s.dht.port, quint16(6881));
        s.dht.enabled = false; s.dht.port = 6969;
        auto back = io.fromJson(io.toJson(s));
        QCOMPARE(back.dht.enabled, false);
        QCOMPARE(back.dht.port, quint16(6969));
    }
```
(Mirror the exact `SettingsIo` API used by existing `tst_settings` cases.)

- [ ] **Step 2: Run, verify fails.**

- [ ] **Step 3: Implement** the `dht` struct in the settings model, JSON read/write with defaults + unknown-key preservation, the Preferences widgets, and live apply (start/stop `DhtNode`, rebind port).

- [ ] **Step 4: Run `tst_settings` + full `ctest`, verify all pass.** Confirm gates (`tst_download`/`tst_ftp`/`tst_torrent`) still green. Do NOT commit.

---

## Final verification (after Task 17)

- [ ] Run the whole suite: `ctest --test-dir build --output-on-failure` → **all green**.
- [ ] Run `tst_dht` and the two E2E torrent cases (Tasks 8, 14, 15) in a 20x loop → **no flake**.
- [ ] Build `orbit-gui` and confirm it launches headless without crash.
- [ ] Human E2E (carlos): add the Ubuntu `.torrent` AND the Ubuntu magnet, confirm both reach peers via DHT and download to completion (the qBittorrent-parity goal from the spec §1.1).

## Self-review notes (coverage against the spec)

- Spec §3 units → Tasks 1 (`MagnetUri`), 4 (`KrpcCodec`), 5-10 (`DhtNode`), 11-12 (extension/ut_metadata), 13 (`MetadataFetch`). ✅
- Spec §4 DHT (NodeId, RoutingTable, KRPC, client/server, bootstrap/DNS, dht.dat, security) → Tasks 2,3,4,5,6,7,8,9,10 (security: KRPC garbage test in Task 4, token validation in Task 7). ✅
- Spec §5 (extension protocol, ut_metadata, MetadataFetch) → Tasks 11,12,13. ✅
- Spec §6 integration (DHT peer source, `addMagnet`, `FetchingMetadata`, resume, GUI, edge cases, settings) → Tasks 14,15,16,17. ✅
- Spec §7 testing (pure units, in-process DHT mesh, metadata peer, magnet E2E) → each task's tests + Final verification. ✅
- Spec §8 deferrals honored (no announce_peer of self, no serving, no PEX). ✅

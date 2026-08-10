# BitTorrent Sub-phase B — UDP trackers + multi-tracker + compact peers — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the from-scratch BitTorrent leech client UDP trackers (BEP 15), multi-tracker `announce-list` (BEP 12), and IPv4+IPv6 compact peer parsing, so it works with the torrents people actually use today.

**Architecture:** A new tracker **interface** (`ITrackerClient`) with two implementations — the existing `HttpTrackerClient` (refactored) and a new `UdpTrackerClient` — behind an `AnnounceController` that owns the announce tiers and presents `TorrentTask` the same two signals it already consumes (`peersReceived` / `announceFailed`). Peer parsing is extracted into a pure `TrackerPeers` unit. `TorrentTask` keeps the announce timer/cadence; the controller only routes *which* trackers to contact.

**Tech Stack:** C++20, Qt 6.11 (Core + Network only in `orbitcore`), `QUdpSocket`, `QtEndian`, QtTest.

## Global Constraints

- **C++20**; `orbitcore` links only `Qt6::Core` + `Qt6::Network` — **no QtWidgets** in the core lib.
- **Event-loop async only** — no threads/mutexes in the torrent subsystem.
- **No RNG / no wall-clock** in the torrent subsystem: anything "random" (UDP `transaction_id`, `key`) is derived deterministically from the existing `rngSeed`; anything time-based (retry/announce cadence) is a pure function of injected inputs. This mirrors how `peer_id` and `nextAnnounceDelaySecs` already work.
- **Leech-only unchanged** — no change to `PeerConnection`, the download/verify engine, resume (`.bitfield`), the GUI, or `DownloadManager`.
- **Gate tests must stay green** on every task: `tst_download`, `tst_ftp`, `tst_torrent`, `tst_metainfo`, `tst_tracker`, `tst_seeder`.
- **NO COMMITS.** Per carlos's standing rule, executors/subagents **never** commit (not even locally). Each task ends by running the gate and leaving the changes in the working tree for carlos to review and commit himself. Replace any "commit" instinct with "run the gate, then stop for review".
- **Build/test commands:**
  - Configure: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`
  - Build: `cmake --build build -j`
  - Run one suite: `ctest --test-dir build -R <name> --output-on-failure`
  - Gate: `ctest --test-dir build -R "tst_download|tst_ftp|tst_torrent|tst_metainfo|tst_tracker|tst_seeder" --output-on-failure`
- Spec: `docs/superpowers/specs/2026-07-24-bittorrent-udp-multitracker-design.md`.

---

## File Structure

**New (core):**
- `src/core/torrent/TrackerTypes.h` — `PeerAddress`, `TrackerEvent`, metatype decls (extracted from `HttpTrackerClient.h`).
- `src/core/torrent/TrackerPeers.h` / `.cpp` — pure peer parsing: compact IPv4/IPv6, bogon filter.
- `src/core/torrent/ITrackerClient.h` — the announce interface (abstract QObject).
- `src/core/torrent/UdpTrackerClient.h` / `.cpp` — BEP 15 client + pure `UdpTrackerProto` packet codecs + timeout.
- `src/core/torrent/AnnounceController.h` / `.cpp` — tier orchestration (BEP 12 + hungry fallback).

**Modified (core):**
- `src/core/torrent/HttpTrackerClient.h` / `.cpp` — implement `ITrackerClient`; tracker URL at construction; drop the synchronous `udp://` rejection; delegate peer extraction to `TrackerPeers`.
- `src/core/torrent/TorrentMetainfo.h` / `.cpp` — add `announceList` (BEP 12) parsing.
- `src/core/torrent/TorrentTask.h` / `.cpp` — replace `m_tracker` (`HttpTrackerClient*`) with `m_announce` (`AnnounceController*`); pass a `hungry` flag when peer-starved.
- `src/core/CMakeLists.txt` — add the new sources.

**New (tests):**
- `tests/TestUdpTracker.h` / `.cpp` — in-process `QUdpSocket` tracker server (mirrors `TestSeeder`/`TestFtpServer`).
- `tests/tst_trackerpeers.cpp`, `tests/tst_udptracker.cpp`, `tests/tst_announcecontroller.cpp`.

**Modified (tests):**
- `tests/tst_metainfo.cpp` — `announce-list` cases.
- `tests/tst_tracker.cpp` — adapt to URL-at-construction.
- `tests/tst_torrent.cpp` — offline E2E through the UDP tracker.
- `tests/CMakeLists.txt` — new executables + link `TestUdpTracker` where used.

---

## Task 1: TrackerPeers — pure IPv4/IPv6 compact parsing + bogon filter

**Files:**
- Create: `src/core/torrent/TrackerTypes.h`
- Create: `src/core/torrent/TrackerPeers.h`, `src/core/torrent/TrackerPeers.cpp`
- Modify: `src/core/torrent/HttpTrackerClient.h` (move types out; include `TrackerTypes.h`)
- Modify: `src/core/torrent/HttpTrackerClient.cpp` (delegate compact parsing + `peers6` + bogon)
- Modify: `src/core/CMakeLists.txt` (add `torrent/TrackerPeers.cpp`)
- Test: `tests/tst_trackerpeers.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct PeerAddress { QString host; quint16 port = 0; };` and `enum class TrackerEvent { None, Started, Stopped, Completed };` — now in `TrackerTypes.h`.
  - `namespace TrackerPeers { QVector<PeerAddress> fromCompactV4(const QByteArray& raw); QVector<PeerAddress> fromCompactV6(const QByteArray& raw); bool isBogon(const PeerAddress& p); void dropBogons(QVector<PeerAddress>& v); }`
- Consumes: nothing new.

- [ ] **Step 1: Move shared types into `TrackerTypes.h`**

Create `src/core/torrent/TrackerTypes.h`:

```cpp
#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

// One peer address as reported by a tracker (compact or dictionary form).
struct PeerAddress {
    QString host;
    quint16 port = 0;
};

// BEP 3 announce event, sent as the tracker's "event" query parameter.
// None omits the parameter entirely (a plain periodic re-announce).
enum class TrackerEvent { None, Started, Stopped, Completed };

Q_DECLARE_METATYPE(PeerAddress)
Q_DECLARE_METATYPE(QVector<PeerAddress>)
```

Edit `src/core/torrent/HttpTrackerClient.h`: delete the `struct PeerAddress`, the `enum class TrackerEvent`, and the two `Q_DECLARE_METATYPE` lines at the bottom; add `#include "torrent/TrackerTypes.h"` near the top (after the Qt includes). Leave everything else (the `TrackerProto` namespace and `HttpTrackerClient`) untouched for now.

- [ ] **Step 2: Write the failing test**

Create `tests/tst_trackerpeers.cpp`:

```cpp
#include "torrent/TrackerPeers.h"

#include <QtTest>

class TestTrackerPeers : public QObject {
    Q_OBJECT
private slots:
    void compactV4_parsesEachSixByteRecord() {
        // 1.2.3.4:0x1a0b (6667), 255.0.0.1:80
        QByteArray raw;
        raw.append(char(1)).append(char(2)).append(char(3)).append(char(4)).append(char(0x1a)).append(char(0x0b));
        raw.append(char(255)).append(char(0)).append(char(0)).append(char(1)).append(char(0)).append(char(80));
        const auto peers = TrackerPeers::fromCompactV4(raw);
        QCOMPARE(peers.size(), 2);
        QCOMPARE(peers[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(0x1a0b));
        QCOMPARE(peers[1].host, QStringLiteral("255.0.0.1"));
        QCOMPARE(peers[1].port, quint16(80));
    }

    void compactV4_ignoresTrailingPartialRecord() {
        QByteArray raw(6 + 3, char(9)); // one full record + 3 stray bytes
        QCOMPARE(TrackerPeers::fromCompactV4(raw).size(), 1);
    }

    void compactV6_parsesEachEighteenByteRecord() {
        QByteArray raw(18, char(0));
        raw[0] = char(0x20); raw[1] = char(0x01); // 2001:...
        raw[15] = char(0x01);                     // ...::1 host-part
        raw[16] = char(0x1a); raw[17] = char(0x0b); // port 6667
        const auto peers = TrackerPeers::fromCompactV6(raw);
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].port, quint16(0x1a0b));
        QVERIFY(peers[0].host.contains(':')); // an IPv6 literal
        QVERIFY(!peers[0].host.contains('[')); // bracketless — QTcpSocket wants it plain
    }

    void isBogon_flagsAlwaysInvalidButKeepsPrivate() {
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("0.0.0.0"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("127.0.0.1"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("::1"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("239.1.2.3"), 6881})); // multicast
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("1.2.3.4"), 0}));       // port 0
        // Private ranges are KEPT (LAN swarms are legitimate):
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("192.168.1.5"), 6881}));
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("10.0.0.9"), 6881}));
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("1.2.3.4"), 6881}));
    }

    void dropBogons_removesInPlace() {
        QVector<PeerAddress> v{{QStringLiteral("1.2.3.4"), 6881},
                               {QStringLiteral("0.0.0.0"), 6881},
                               {QStringLiteral("10.0.0.9"), 6881}};
        TrackerPeers::dropBogons(v);
        QCOMPARE(v.size(), 2);
        QCOMPARE(v[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(v[1].host, QStringLiteral("10.0.0.9"));
    }
};

QTEST_MAIN(TestTrackerPeers)
#include "tst_trackerpeers.moc"
```

- [ ] **Step 3: Run test to verify it fails**

Add to `tests/CMakeLists.txt` (next to `tst_tracker`):

```cmake
add_executable(tst_trackerpeers tst_trackerpeers.cpp)
target_link_libraries(tst_trackerpeers PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_trackerpeers COMMAND tst_trackerpeers)
```

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build -j 2>&1 | head -20`
Expected: FAIL — `TrackerPeers.h` not found / undefined symbols.

- [ ] **Step 4: Write minimal implementation**

Create `src/core/torrent/TrackerPeers.h`:

```cpp
#pragma once

#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QVector>

// Pure peer-list parsing shared by the HTTP and UDP tracker clients. No Qt
// networking, no Bencode: it turns raw compact records into PeerAddress values
// and filters addresses that can never be a real peer.
namespace TrackerPeers {

// Compact IPv4 (BEP 23): 6 bytes per record — 4-byte big-endian IPv4 + 2-byte
// big-endian port. A trailing partial (<6 byte) record is ignored, not an error.
QVector<PeerAddress> fromCompactV4(const QByteArray& raw);

// Compact IPv6 (BEP 7): 18 bytes per record — 16-byte IPv6 + 2-byte big-endian
// port. The host is rendered as a bracketless literal (QTcpSocket wants it plain).
QVector<PeerAddress> fromCompactV6(const QByteArray& raw);

// True for addresses that can never be a real peer: 0.0.0.0/8, loopback
// (127/8, ::1), multicast (224/4, ff00::/8), 255.255.255.255, unspecified ::,
// or port 0. Deliberately does NOT reject RFC1918/link-local — LAN peers are
// legitimate, and an unreachable one merely fails a single TCP connect.
bool isBogon(const PeerAddress& p);

// Removes every isBogon() entry from v in place, preserving order.
void dropBogons(QVector<PeerAddress>& v);

} // namespace TrackerPeers
```

Create `src/core/torrent/TrackerPeers.cpp`:

```cpp
#include "torrent/TrackerPeers.h"

#include <QHostAddress>

namespace {

quint16 be16(const QByteArray& b, int i) {
    return quint16((quint8(b[i]) << 8) | quint8(b[i + 1]));
}

} // namespace

QVector<PeerAddress> TrackerPeers::fromCompactV4(const QByteArray& raw) {
    QVector<PeerAddress> out;
    for (int i = 0; i + 6 <= raw.size(); i += 6) {
        const QString host = QStringLiteral("%1.%2.%3.%4")
            .arg(quint8(raw[i])).arg(quint8(raw[i + 1]))
            .arg(quint8(raw[i + 2])).arg(quint8(raw[i + 3]));
        out.append(PeerAddress{host, be16(raw, i + 4)});
    }
    return out;
}

QVector<PeerAddress> TrackerPeers::fromCompactV6(const QByteArray& raw) {
    QVector<PeerAddress> out;
    for (int i = 0; i + 18 <= raw.size(); i += 18) {
        Q_IPV6ADDR addr;
        for (int j = 0; j < 16; ++j) addr[j] = quint8(raw[i + j]);
        const QString host = QHostAddress(addr).toString(); // bracketless literal
        out.append(PeerAddress{host, be16(raw, i + 16)});
    }
    return out;
}

bool TrackerPeers::isBogon(const PeerAddress& p) {
    if (p.port == 0) return true;
    QHostAddress a(p.host);
    if (a.isNull()) return true; // unparseable
    if (a == QHostAddress(QHostAddress::AnyIPv4) || a == QHostAddress(QHostAddress::AnyIPv6))
        return true; // 0.0.0.0 / ::
    if (a.isLoopback() || a.isMulticast() || a.isBroadcast()) return true;
    if (a.protocol() == QAbstractSocket::IPv4Protocol && (a.toIPv4Address() >> 24) == 0)
        return true; // 0.0.0.0/8
    return false;
}

void TrackerPeers::dropBogons(QVector<PeerAddress>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const PeerAddress& p) { return isBogon(p); }),
            v.end());
}
```

Add `torrent/TrackerPeers.cpp` to `src/core/CMakeLists.txt` in the `orbitcore` source list (after `torrent/TorrentMetainfo.cpp`).

- [ ] **Step 5: Run the new test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R tst_trackerpeers --output-on-failure`
Expected: PASS (5 slots).

- [ ] **Step 6: Rewire `TrackerProto::parseResponse` to use `TrackerPeers` (compact v4 + `peers6` + dict + bogon)**

In `src/core/torrent/HttpTrackerClient.cpp`, add `#include "torrent/TrackerPeers.h"` near the top. Replace the whole peer-extraction block in `parseResponse` (the `if (root.contains("peers")) { ... }` region) with:

```cpp
    if (peers) {
        peers->clear();
        if (root.contains("peers")) {
            const BencodeValue& pv = root[QByteArray("peers")];
            if (pv.type() == BencodeValue::Type::Bytes) {
                *peers += TrackerPeers::fromCompactV4(pv.toBytes());
            } else if (pv.type() == BencodeValue::Type::List) {
                for (const BencodeValue& entry : pv.toList()) {
                    if (entry.type() != BencodeValue::Type::Dict) continue;
                    const QString host = entry.contains("ip")
                        ? QString::fromUtf8(entry[QByteArray("ip")].toBytes()) : QString();
                    const quint16 port = entry.contains("port")
                        ? quint16(entry[QByteArray("port")].toInt()) : 0;
                    peers->append(PeerAddress{host, port});
                }
            }
        }
        if (root.contains("peers6") && root[QByteArray("peers6")].type() == BencodeValue::Type::Bytes)
            *peers += TrackerPeers::fromCompactV6(root[QByteArray("peers6")].toBytes());
        TrackerPeers::dropBogons(*peers);
    }
```

- [ ] **Step 7: Run the gate to confirm no regression**

Run: `cmake --build build -j && ctest --test-dir build -R "tst_trackerpeers|tst_tracker|tst_torrent|tst_metainfo" --output-on-failure`
Expected: all PASS. (`tst_tracker`'s existing compact/dict cases still pass; bogon filtering only drops all-zero/loopback which those fixtures don't use — if a fixture does, adjust that fixture's IPs to non-bogon values.)

- [ ] **Step 8: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 2: TorrentMetainfo — `announce-list` (BEP 12) parsing

**Files:**
- Modify: `src/core/torrent/TorrentMetainfo.h` (add `announceList`)
- Modify: `src/core/torrent/TorrentMetainfo.cpp` (parse `announce-list`, synthesize fallback)
- Test: `tests/tst_metainfo.cpp`

**Interfaces:**
- Produces: `QVector<QVector<QUrl>> TorrentMetainfo::announceList;` — tiers of tracker URLs. Always non-empty on success when at least one usable tracker exists: if `announce-list` is absent/empty, it holds a single tier `[[announce]]`.
- Consumes: nothing new.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_metainfo.cpp` (inside the test class' private slots). Use the file's existing bencode-building helper if present; otherwise build bytes inline as below:

```cpp
    void announceList_parsedIntoTiers() {
        // A minimal valid single-file torrent with announce + announce-list.
        // info = { name:"f", piece length:16384, pieces:<20 bytes>, length:1 }
        QByteArray info = "d6:lengthi1e4:name1:f12:piece lengthi16384e6:pieces20:";
        info += QByteArray(20, char(0xAB));
        info += "e";
        QByteArray meta = "d8:announce20:http://primary/annou13:announce-list"
                          "ll20:http://primary/annou el21:udp://backup.example:9 ee"
                          "4:info";
        meta += info;
        meta += "e";
        bool ok = false; QString err;
        const TorrentMetainfo m = TorrentMetainfo::parse(meta, &ok, &err);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(m.announceList.size(), 2);              // two tiers
        QCOMPARE(m.announceList[0].size(), 1);
        QCOMPARE(m.announceList[0][0], QUrl("http://primary/annou"));
        QCOMPARE(m.announceList[1][0], QUrl("udp://backup.example:9"));
    }

    void announceList_absent_fallsBackToAnnounce() {
        QByteArray info = "d6:lengthi1e4:name1:f12:piece lengthi16384e6:pieces20:";
        info += QByteArray(20, char(0xAB));
        info += "e";
        QByteArray meta = "d8:announce17:http://only/annou4:info";
        meta += info; meta += "e";
        bool ok = false; QString err;
        const TorrentMetainfo m = TorrentMetainfo::parse(meta, &ok, &err);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(m.announceList.size(), 1);
        QCOMPARE(m.announceList[0].size(), 1);
        QCOMPARE(m.announceList[0][0], QUrl("http://only/annou"));
    }
```

> Note to implementer: the exact bencode string lengths above are illustrative. Build the fixtures using the same helper the rest of `tst_metainfo.cpp` uses (grep the file for how it constructs `info`/`meta`), so the byte-length prefixes are correct. The **assertions** are the contract; keep them.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R tst_metainfo --output-on-failure`
Expected: FAIL — `announceList` has no member / size 0.

- [ ] **Step 3: Add the field**

In `src/core/torrent/TorrentMetainfo.h`, add to the struct (after `QUrl announce;`):

```cpp
    QVector<QVector<QUrl>> announceList; // BEP 12 tiers; [[announce]] when the key is absent
```

- [ ] **Step 4: Parse `announce-list` and synthesize the fallback**

In `src/core/torrent/TorrentMetainfo.cpp`, immediately **after** the `m.announce = QUrl(...)` line (the current line ~199, before `if (ok) *ok = true;`), insert:

```cpp
    // BEP 12 announce-list: a list of tiers, each tier a list of tracker URL
    // byte-strings. Skip malformed entries/empty tiers. When absent or empty,
    // synthesize a single tier holding the plain `announce` URL so downstream
    // code always sees a uniform tier structure.
    if (root.contains(QByteArray("announce-list")) &&
        root[QByteArray("announce-list")].type() == BencodeValue::Type::List) {
        for (const BencodeValue& tierVal : root[QByteArray("announce-list")].toList()) {
            if (tierVal.type() != BencodeValue::Type::List) continue;
            QVector<QUrl> tier;
            for (const BencodeValue& urlVal : tierVal.toList()) {
                if (urlVal.type() != BencodeValue::Type::Bytes) continue;
                const QUrl u(QString::fromUtf8(urlVal.toBytes()));
                if (u.isValid() && !u.scheme().isEmpty()) tier.append(u);
            }
            if (!tier.isEmpty()) m.announceList.append(tier);
        }
    }
    if (m.announceList.isEmpty() && m.announce.isValid() && !m.announce.scheme().isEmpty())
        m.announceList.append(QVector<QUrl>{m.announce});
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R tst_metainfo --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 3: ITrackerClient interface + HttpTrackerClient refactor

**Files:**
- Create: `src/core/torrent/ITrackerClient.h`
- Modify: `src/core/torrent/HttpTrackerClient.h` (inherit `ITrackerClient`; URL at construction)
- Modify: `src/core/torrent/HttpTrackerClient.cpp` (constructor stores URL; `announce` drops URL param + `udp` rejection)
- Modify: `tests/tst_tracker.cpp` (construct with URL; call `announce` without URL)

**Interfaces:**
- Produces:
  - `class ITrackerClient : public QObject` with pure virtual `void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev)` and `QUrl trackerUrl() const`; signals `peersReceived(QVector<PeerAddress>, int intervalSecs, int minIntervalSecs)` and `announceFailed(QString)`.
  - `HttpTrackerClient(QNetworkAccessManager* nam, const QUrl& tracker, QObject* parent=nullptr)` implementing it.
- Consumes: `TrackerEvent`, `PeerAddress` from `TrackerTypes.h` (Task 1); `TrackerProto::buildAnnounceUrl` / `parseResponse` (unchanged).

- [ ] **Step 1: Create the interface**

Create `src/core/torrent/ITrackerClient.h`:

```cpp
#pragma once

#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <QVector>

// Common contract for a single tracker (one instance = one tracker URL).
// AnnounceController owns a set of these and routes announces across them;
// TorrentTask never sees a concrete client, only the controller.
class ITrackerClient : public QObject {
    Q_OBJECT
public:
    explicit ITrackerClient(QObject* parent = nullptr) : QObject(parent) {}
    ~ITrackerClient() override = default;

    virtual QUrl trackerUrl() const = 0;

    // Fire one announce to this client's tracker. Emits peersReceived on a
    // well-formed response, announceFailed otherwise.
    virtual void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                          qint64 downloaded, qint64 left, TrackerEvent ev) = 0;

signals:
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);
};
```

- [ ] **Step 2: Refactor HttpTrackerClient onto the interface**

Edit `src/core/torrent/HttpTrackerClient.h`:
- Add `#include "torrent/ITrackerClient.h"`.
- Change the class to `class HttpTrackerClient : public ITrackerClient {`.
- Replace the constructor + `announce` declarations with:

```cpp
public:
    HttpTrackerClient(QNetworkAccessManager* nam, const QUrl& tracker, QObject* parent = nullptr);

    QUrl trackerUrl() const override { return m_tracker; }
    void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                  qint64 downloaded, qint64 left, TrackerEvent ev) override;
```
- Delete the old `signals:` block (now inherited from `ITrackerClient`).
- Add to the private members: `QUrl m_tracker;`.
- Keep the `namespace TrackerProto { ... }` block as-is.

Edit `src/core/torrent/HttpTrackerClient.cpp`:
- Constructor:

```cpp
HttpTrackerClient::HttpTrackerClient(QNetworkAccessManager* nam, const QUrl& tracker, QObject* parent)
    : ITrackerClient(parent), m_nam(nam), m_tracker(tracker) {}
```
- `announce`: drop the `tracker` parameter and the `udp://` rejection; use `m_tracker`:

```cpp
void HttpTrackerClient::announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                                 qint64 downloaded, qint64 left, TrackerEvent ev) {
    const QUrl url = TrackerProto::buildAnnounceUrl(m_tracker, infoHash, peerId, port, downloaded, left, ev);
    QNetworkReply* reply = m_nam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) { emit announceFailed(reply->errorString()); return; }
        const QByteArray body = reply->readAll();
        QVector<PeerAddress> peers; int interval = 0; int minInterval = 0; QString failure;
        if (!TrackerProto::parseResponse(body, &peers, &interval, &minInterval, &failure)) {
            emit announceFailed(failure); return;
        }
        emit peersReceived(peers, interval, minInterval);
    });
}
```

> The `udp://` scheme is now handled by AnnounceController's factory (Task 6), which routes `udp` to `UdpTrackerClient`. HttpTrackerClient is never constructed with a `udp` URL.

- [ ] **Step 3: Update tst_tracker for the new signatures**

In `tests/tst_tracker.cpp`, find each `HttpTrackerClient` construction and `announce(...)` call:
- Change `HttpTrackerClient client(&nam);` (or similar) to `HttpTrackerClient client(&nam, trackerUrl);` where `trackerUrl` is the `QUrl` previously passed to `announce`.
- Change `client.announce(url, infoHash, peerId, port, dl, left, ev);` to `client.announce(infoHash, peerId, port, dl, left, ev);`.
- If a test constructed a `udp://` client to assert the synchronous rejection, **delete that test** (the behavior moved to the controller and is covered in Task 6); note the deletion in the review report.

- [ ] **Step 4: Build and run tst_tracker**

Run: `cmake --build build -j && ctest --test-dir build -R tst_tracker --output-on-failure`
Expected: PASS (adapted cases).

- [ ] **Step 5: Run the gate**

Run: `ctest --test-dir build -R "tst_tracker|tst_torrent|tst_download|tst_ftp" --output-on-failure`
Expected: `tst_tracker` PASS. `tst_torrent`/`tst_download`/`tst_ftp` **will not build yet** if `TorrentTask` still constructs `HttpTrackerClient(m_nam)` — that wiring is Task 7. If the torrent target fails to compile here, that is expected; confirm `tst_tracker` passes in isolation and note the pending `TorrentTask` wiring. (Do not touch `TorrentTask` in this task.)

> Right-sizing note: to keep every task's gate green, do a **minimal compile-fix** in `src/core/torrent/TorrentTask.cpp` here: change the two `new HttpTrackerClient(m_nam, this)` / `announce(m_meta.announce, ...)` call sites to `new HttpTrackerClient(m_nam, m_meta.announce, this)` and drop the leading `m_meta.announce,` argument from the two `->announce(...)` calls. This is a mechanical adapter so the tree keeps compiling; Task 7 replaces it wholesale with the controller. After this fix, the full gate should pass.

- [ ] **Step 6: Re-run the gate after the compile-fix**

Run: `ctest --test-dir build -R "tst_tracker|tst_torrent|tst_download|tst_ftp" --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 4: UdpTrackerProto — pure BEP 15 packet codecs + timeout

**Files:**
- Create: `src/core/torrent/UdpTrackerClient.h` (declare `namespace UdpTrackerProto` + the class stub for Task 5)
- Create: `src/core/torrent/UdpTrackerClient.cpp` (implement `UdpTrackerProto`; class in Task 5)
- Modify: `src/core/CMakeLists.txt` (add `torrent/UdpTrackerClient.cpp`)
- Test: `tests/tst_udptracker.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces (`namespace UdpTrackerProto`):
  - `constexpr quint64 kProtocolId = 0x41727101980ULL;`
  - `quint32 eventCode(TrackerEvent ev);` — none=0, completed=1, started=2, stopped=3.
  - `QByteArray buildConnectRequest(quint32 txId);`
  - `bool parseConnectResponse(const QByteArray& dg, quint32 expectTxId, quint64* connId);`
  - `QByteArray buildAnnounceRequest(quint64 connId, quint32 txId, const QByteArray& infoHash, const QByteArray& peerId, qint64 downloaded, qint64 left, qint64 uploaded, TrackerEvent ev, quint32 key, quint16 port);`
  - `bool parseAnnounceResponse(const QByteArray& dg, quint32 expectTxId, int* intervalSecs, QVector<PeerAddress>* peers, QString* errorMsg);` — returns false and sets `*errorMsg` on an action-3 error datagram or a malformed/short one; sets `*peers` (bogon-filtered) + `*intervalSecs` on success.
  - `int timeoutSecs(int attempt);` — 15, 30, 60 for attempt 0,1,2; caller gives up after attempt 2.
- Consumes: `TrackerEvent`, `PeerAddress`, `TrackerPeers` (Task 1).

- [ ] **Step 1: Write the failing test**

Create `tests/tst_udptracker.cpp` (the pure section — live socket tests come in Task 5):

```cpp
#include "torrent/UdpTrackerClient.h"

#include <QtEndian>
#include <QtTest>

class TestUdpTracker : public QObject {
    Q_OBJECT
private slots:
    void connectRequest_hasMagicActionAndTxId() {
        const QByteArray req = UdpTrackerProto::buildConnectRequest(0x11223344u);
        QCOMPARE(req.size(), 16);
        QCOMPARE(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(req.constData())),
                 UdpTrackerProto::kProtocolId);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 8)), 0u); // action=connect
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 12)), 0x11223344u);
    }

    void parseConnectResponse_extractsConnectionId() {
        QByteArray dg(16, char(0));
        qToBigEndian<quint32>(0u, reinterpret_cast<uchar*>(dg.data()));       // action=connect
        qToBigEndian<quint32>(0x11223344u, reinterpret_cast<uchar*>(dg.data() + 4)); // txId
        qToBigEndian<quint64>(0xDEADBEEFCAFEULL, reinterpret_cast<uchar*>(dg.data() + 8));
        quint64 connId = 0;
        QVERIFY(UdpTrackerProto::parseConnectResponse(dg, 0x11223344u, &connId));
        QCOMPARE(connId, 0xDEADBEEFCAFEULL);
    }

    void parseConnectResponse_rejectsWrongTxId() {
        QByteArray dg(16, char(0));
        qToBigEndian<quint32>(0x99u, reinterpret_cast<uchar*>(dg.data() + 4));
        quint64 connId = 0;
        QVERIFY(!UdpTrackerProto::parseConnectResponse(dg, 0x11223344u, &connId));
    }

    void announceRequest_layoutIsBep15() {
        const QByteArray ih(20, char(0xA1)), pid(20, char(0xB2));
        const QByteArray req = UdpTrackerProto::buildAnnounceRequest(
            0xCAFEULL, 0x2222u, ih, pid, /*dl*/100, /*left*/200, /*up*/0,
            TrackerEvent::Started, /*key*/0x9999u, /*port*/6881);
        QCOMPARE(req.size(), 98);
        QCOMPARE(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(req.constData())), 0xCAFEULL);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 8)), 1u); // action=announce
        QCOMPARE(req.mid(16, 20), ih);
        QCOMPARE(req.mid(36, 20), pid);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 80)), 2u); // event=started
        QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(req.constData() + 96)), quint16(6881));
    }

    void parseAnnounceResponse_extractsIntervalAndPeers() {
        QByteArray dg(20, char(0));
        qToBigEndian<quint32>(1u, reinterpret_cast<uchar*>(dg.data()));        // action=announce
        qToBigEndian<quint32>(0x2222u, reinterpret_cast<uchar*>(dg.data() + 4)); // txId
        qToBigEndian<quint32>(1800u, reinterpret_cast<uchar*>(dg.data() + 8));  // interval
        // one compact peer 1.2.3.4:6881
        QByteArray peer; peer.append(char(1)).append(char(2)).append(char(3)).append(char(4));
        qToBigEndian<quint16>(quint16(6881), reinterpret_cast<uchar*>(peer.data() + 4)); // fill port
        peer.resize(6);
        peer[4] = char((6881 >> 8) & 0xFF); peer[5] = char(6881 & 0xFF);
        dg += peer;
        int interval = 0; QVector<PeerAddress> peers; QString err;
        QVERIFY(UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err));
        QCOMPARE(interval, 1800);
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(6881));
    }

    void parseAnnounceResponse_reportsErrorAction() {
        QByteArray dg(8, char(0));
        qToBigEndian<quint32>(3u, reinterpret_cast<uchar*>(dg.data()));        // action=error
        qToBigEndian<quint32>(0x2222u, reinterpret_cast<uchar*>(dg.data() + 4));
        dg += "bad request";
        int interval = 0; QVector<PeerAddress> peers; QString err;
        QVERIFY(!UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err));
        QCOMPARE(err, QStringLiteral("bad request"));
    }

    void timeoutSecs_bep15CappedSchedule() {
        QCOMPARE(UdpTrackerProto::timeoutSecs(0), 15);
        QCOMPARE(UdpTrackerProto::timeoutSecs(1), 30);
        QCOMPARE(UdpTrackerProto::timeoutSecs(2), 60);
    }
};

QTEST_MAIN(TestUdpTracker)
#include "tst_udptracker.moc"
```

- [ ] **Step 2: Run to verify it fails**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_udptracker tst_udptracker.cpp)
target_link_libraries(tst_udptracker PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_udptracker COMMAND tst_udptracker)
```

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build -j 2>&1 | head -20`
Expected: FAIL — `UdpTrackerClient.h` not found.

- [ ] **Step 3: Create the header (proto + class stub)**

Create `src/core/torrent/UdpTrackerClient.h`:

```cpp
#pragma once

#include "torrent/ITrackerClient.h"
#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QHostInfo>
#include <QUrl>
#include <QVector>

class QUdpSocket;
class QTimer;

// Pure BEP 15 packet codecs + retry schedule. No sockets: fully unit-testable.
namespace UdpTrackerProto {

constexpr quint64 kProtocolId = 0x41727101980ULL;

quint32     eventCode(TrackerEvent ev); // none=0, completed=1, started=2, stopped=3
QByteArray  buildConnectRequest(quint32 txId);
bool        parseConnectResponse(const QByteArray& dg, quint32 expectTxId, quint64* connId);
QByteArray  buildAnnounceRequest(quint64 connId, quint32 txId, const QByteArray& infoHash,
                                 const QByteArray& peerId, qint64 downloaded, qint64 left,
                                 qint64 uploaded, TrackerEvent ev, quint32 key, quint16 port);
// On an action-3 error datagram sets *errorMsg and returns false. On a valid
// announce reply fills *intervalSecs and *peers (bogon-filtered) and returns
// true. A short/malformed datagram returns false with a generic *errorMsg.
bool        parseAnnounceResponse(const QByteArray& dg, quint32 expectTxId, int* intervalSecs,
                                  QVector<PeerAddress>* peers, QString* errorMsg);
int         timeoutSecs(int attempt); // 15, 30, 60 (attempt 0..2); caller gives up after 2

} // namespace UdpTrackerProto

// UDP BEP 15 tracker client — declared here, implemented in Task 5.
class UdpTrackerClient : public ITrackerClient {
    Q_OBJECT
public:
    UdpTrackerClient(const QUrl& tracker, quint32 rngSeed, QObject* parent = nullptr);
    ~UdpTrackerClient() override;

    QUrl trackerUrl() const override { return m_tracker; }
    void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                  qint64 downloaded, qint64 left, TrackerEvent ev) override;

private:
    // Task 5 fills these in.
    QUrl        m_tracker;
    quint32     m_key = 0;
    quint32     m_txCounter = 0;
    QUdpSocket* m_sock = nullptr;
    QTimer*     m_timeout = nullptr;
};
```

- [ ] **Step 4: Implement `UdpTrackerProto` (class methods are Task 5 — leave them out for now)**

Create `src/core/torrent/UdpTrackerClient.cpp`:

```cpp
#include "torrent/UdpTrackerClient.h"

#include "torrent/TrackerPeers.h"

#include <QtEndian>

quint32 UdpTrackerProto::eventCode(TrackerEvent ev) {
    switch (ev) {
    case TrackerEvent::None:      return 0;
    case TrackerEvent::Completed: return 1;
    case TrackerEvent::Started:   return 2;
    case TrackerEvent::Stopped:   return 3;
    }
    return 0;
}

QByteArray UdpTrackerProto::buildConnectRequest(quint32 txId) {
    QByteArray b(16, char(0));
    auto* p = reinterpret_cast<uchar*>(b.data());
    qToBigEndian<quint64>(kProtocolId, p);
    qToBigEndian<quint32>(0u, p + 8);   // action = connect
    qToBigEndian<quint32>(txId, p + 12);
    return b;
}

bool UdpTrackerProto::parseConnectResponse(const QByteArray& dg, quint32 expectTxId, quint64* connId) {
    if (dg.size() < 16) return false;
    const auto* p = reinterpret_cast<const uchar*>(dg.constData());
    if (qFromBigEndian<quint32>(p) != 0u) return false;                 // action must be connect
    if (qFromBigEndian<quint32>(p + 4) != expectTxId) return false;
    if (connId) *connId = qFromBigEndian<quint64>(p + 8);
    return true;
}

QByteArray UdpTrackerProto::buildAnnounceRequest(quint64 connId, quint32 txId, const QByteArray& infoHash,
                                                 const QByteArray& peerId, qint64 downloaded, qint64 left,
                                                 qint64 uploaded, TrackerEvent ev, quint32 key, quint16 port) {
    QByteArray b(98, char(0));
    auto* p = reinterpret_cast<uchar*>(b.data());
    qToBigEndian<quint64>(connId, p);
    qToBigEndian<quint32>(1u, p + 8);    // action = announce
    qToBigEndian<quint32>(txId, p + 12);
    // info_hash[20] @16, peer_id[20] @36 — copy exactly 20 bytes each (defensive).
    memcpy(b.data() + 16, infoHash.constData(), qMin(20, infoHash.size()));
    memcpy(b.data() + 36, peerId.constData(), qMin(20, peerId.size()));
    qToBigEndian<quint64>(quint64(downloaded), p + 56);
    qToBigEndian<quint64>(quint64(left), p + 64);
    qToBigEndian<quint64>(quint64(uploaded), p + 72);
    qToBigEndian<quint32>(eventCode(ev), p + 80);
    qToBigEndian<quint32>(0u, p + 84);   // IP = 0 (tracker uses source addr)
    qToBigEndian<quint32>(key, p + 88);
    qToBigEndian<qint32>(-1, p + 92);    // num_want = -1 (default)
    qToBigEndian<quint16>(port, p + 96);
    return b;
}

bool UdpTrackerProto::parseAnnounceResponse(const QByteArray& dg, quint32 expectTxId, int* intervalSecs,
                                            QVector<PeerAddress>* peers, QString* errorMsg) {
    if (dg.size() < 8) { if (errorMsg) *errorMsg = QStringLiteral("short datagram"); return false; }
    const auto* p = reinterpret_cast<const uchar*>(dg.constData());
    const quint32 action = qFromBigEndian<quint32>(p);
    if (qFromBigEndian<quint32>(p + 4) != expectTxId) {
        if (errorMsg) *errorMsg = QStringLiteral("transaction id mismatch");
        return false;
    }
    if (action == 3u) { // error
        if (errorMsg) *errorMsg = QString::fromUtf8(dg.mid(8));
        return false;
    }
    if (action != 1u || dg.size() < 20) {
        if (errorMsg) *errorMsg = QStringLiteral("malformed announce response");
        return false;
    }
    if (intervalSecs) *intervalSecs = int(qFromBigEndian<quint32>(p + 8));
    // leechers @12, seeders @16, peers (compact v4) from @20.
    if (peers) {
        *peers = TrackerPeers::fromCompactV4(dg.mid(20));
        TrackerPeers::dropBogons(*peers);
    }
    return true;
}

int UdpTrackerProto::timeoutSecs(int attempt) {
    static const int schedule[] = {15, 30, 60};
    if (attempt < 0) attempt = 0;
    if (attempt > 2) attempt = 2;
    return schedule[attempt];
}
```

Add `torrent/UdpTrackerClient.cpp` to `src/core/CMakeLists.txt` (after `torrent/TrackerPeers.cpp`).

> Linker note: `UdpTrackerClient`'s constructor/destructor/`announce` are declared but not yet defined. To let `tst_udptracker` link now (it only calls `UdpTrackerProto`), also add **stub definitions** at the end of the `.cpp` so the vtable resolves:
> ```cpp
> UdpTrackerClient::UdpTrackerClient(const QUrl& tracker, quint32 rngSeed, QObject* parent)
>     : ITrackerClient(parent), m_tracker(tracker), m_key(rngSeed) {}
> UdpTrackerClient::~UdpTrackerClient() = default;
> void UdpTrackerClient::announce(const QByteArray&, const QByteArray&, quint16, qint64, qint64, TrackerEvent) {}
> ```
> Task 5 replaces these stubs with the real socket logic.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R tst_udptracker --output-on-failure`
Expected: PASS (7 slots).

- [ ] **Step 6: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 5: UdpTrackerClient (live) + in-process UDP tracker test server

**Files:**
- Modify: `src/core/torrent/UdpTrackerClient.cpp` (replace the Task-4 stubs with real connect→announce logic)
- Create: `tests/TestUdpTracker.h`, `tests/TestUdpTracker.cpp`
- Modify: `tests/tst_udptracker.cpp` (add live cases)
- Modify: `tests/CMakeLists.txt` (link `TestUdpTracker.cpp` into `tst_udptracker`)

**Interfaces:**
- Produces:
  - `class TestUdpTracker` — a `QUdpSocket` server: `quint16 port() const;` and `void setPeers(const QVector<PeerAddress>&);` and `void setInterval(int);`. Answers BEP 15 connect (fixed connection id) + announce (returns the configured peers/interval).
  - Working `UdpTrackerClient::announce(...)` emitting `peersReceived` / `announceFailed`.
- Consumes: `UdpTrackerProto` (Task 4), `ITrackerClient` (Task 3).

- [ ] **Step 1: Write the failing live test**

Add to `tests/tst_udptracker.cpp` (new slots; add `#include "TestUdpTracker.h"` and `#include <QSignalSpy>`):

```cpp
    void liveAnnounce_returnsConfiguredPeers() {
        TestUdpTracker server;
        server.setPeers({{QStringLiteral("9.8.7.6"), 6881}});
        server.setInterval(1200);

        const QUrl url(QStringLiteral("udp://127.0.0.1:%1").arg(server.port()));
        UdpTrackerClient client(url, /*rngSeed*/12345u);
        QSignalSpy ok(&client, &ITrackerClient::peersReceived);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);

        client.announce(QByteArray(20, char(0xA1)), QByteArray(20, char(0xB2)), 6881, 0, 100, TrackerEvent::Started);
        QVERIFY(ok.wait(4000));
        QCOMPARE(bad.count(), 0);
        const auto args = ok.takeFirst();
        const auto peers = args[0].value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("9.8.7.6"));
        QCOMPARE(args[1].toInt(), 1200); // interval
    }

    void liveAnnounce_failsOnSilentTracker() {
        // Point at a closed port; the client must give up (capped retries) and
        // emit announceFailed rather than hang forever.
        UdpTrackerClient client(QUrl(QStringLiteral("udp://127.0.0.1:1")), 1u);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);
        client.announce(QByteArray(20, char(0)), QByteArray(20, char(0)), 6881, 0, 1, TrackerEvent::Started);
        // With the test override below, the retry schedule is compressed so this
        // resolves quickly; see UdpTrackerClient's ORBIT_UDP_FAST_TIMEOUT seam.
        QVERIFY(bad.wait(6000));
    }
```

> The second test would take 15+30+60s with the real BEP 15 schedule. Add a test seam in `UdpTrackerClient` (Step 2): when the env var `ORBIT_UDP_FAST_TIMEOUT` is set, use a compressed schedule (e.g. 200/300/400 ms) instead of `UdpTrackerProto::timeoutSecs`. Set it at the top of this test with `qputenv("ORBIT_UDP_FAST_TIMEOUT", "1");` in an `initTestCase()` slot (and `qunsetenv` in `cleanupTestCase()`). This keeps the pure schedule authoritative while the live test stays fast.

- [ ] **Step 2: Implement the live client**

Replace the three stub definitions at the end of `src/core/torrent/UdpTrackerClient.cpp` with the real implementation. Add includes at the top: `#include <QUdpSocket>`, `#include <QTimer>`, `#include <QHostAddress>`, `#include <QNetworkDatagram>`.

```cpp
namespace {
// Deterministic per-instance transaction id: seeded counter, no RNG (matches the
// torrent subsystem's no-RNG rule). Only needs to correlate request<->response.
} // namespace

UdpTrackerClient::UdpTrackerClient(const QUrl& tracker, quint32 rngSeed, QObject* parent)
    : ITrackerClient(parent), m_tracker(tracker), m_key(rngSeed ^ 0x9E3779B9u) {}

UdpTrackerClient::~UdpTrackerClient() = default;

void UdpTrackerClient::announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                                qint64 downloaded, qint64 left, TrackerEvent ev) {
    // One announce = one connect->announce exchange over a fresh datagram flow.
    // State for this exchange lives in a small heap struct kept alive by lambdas
    // captured on the socket; it self-destructs on success/failure/give-up.
    struct Exchange {
        UdpTrackerClient* self;
        QByteArray infoHash, peerId;
        quint16 port; qint64 downloaded, left; TrackerEvent ev;
        QUdpSocket* sock = nullptr;
        QTimer* timer = nullptr;
        quint32 txId = 0;
        quint64 connId = 0;
        int attempt = 0;
        enum { Connecting, Announcing } phase = Connecting;
    };

    const bool fast = qEnvironmentVariableIsSet("ORBIT_UDP_FAST_TIMEOUT");
    auto* ex = new Exchange{this, infoHash, peerId, port, downloaded, left, ev};
    ex->sock = new QUdpSocket(this);
    ex->timer = new QTimer(this);
    ex->timer->setSingleShot(true);
    ex->txId = ++m_txCounter ^ m_key; // deterministic, per-exchange

    const QString host = m_tracker.host();
    const quint16 tport = quint16(m_tracker.port(80));

    auto cleanup = [ex]() { ex->sock->deleteLater(); ex->timer->deleteLater(); delete ex; };
    auto timeoutMs = [fast](int attempt) -> int {
        return fast ? (200 + 100 * attempt) : UdpTrackerProto::timeoutSecs(attempt) * 1000;
    };

    auto sendConnect = [=]() {
        ex->phase = Exchange::Connecting;
        ex->sock->writeDatagram(UdpTrackerProto::buildConnectRequest(ex->txId),
                                QHostAddress(host), tport);
        ex->timer->start(timeoutMs(ex->attempt));
    };
    auto sendAnnounce = [=]() {
        ex->phase = Exchange::Announcing;
        ex->sock->writeDatagram(
            UdpTrackerProto::buildAnnounceRequest(ex->connId, ex->txId, ex->infoHash, ex->peerId,
                                                  ex->downloaded, ex->left, 0, ex->ev, m_key, ex->port),
            QHostAddress(host), tport);
        ex->timer->start(timeoutMs(ex->attempt));
    };

    connect(ex->timer, &QTimer::timeout, this, [=]() {
        if (++ex->attempt > 2) { // BEP 15 give-up after the capped retries
            emit announceFailed(QStringLiteral("udp tracker timed out: %1").arg(m_tracker.toString()));
            cleanup();
            return;
        }
        ex->txId = ++m_txCounter ^ m_key;
        if (ex->phase == Exchange::Connecting) sendConnect(); else sendAnnounce();
    });

    connect(ex->sock, &QUdpSocket::readyRead, this, [=]() {
        while (ex->sock->hasPendingDatagrams()) {
            const QByteArray dg = ex->sock->receiveDatagram().data();
            if (ex->phase == Exchange::Connecting) {
                quint64 cid = 0;
                if (!UdpTrackerProto::parseConnectResponse(dg, ex->txId, &cid)) continue; // ignore junk
                ex->timer->stop();
                ex->connId = cid;
                ex->attempt = 0;
                ex->txId = ++m_txCounter ^ m_key;
                sendAnnounce();
            } else {
                int interval = 0; QVector<PeerAddress> peers; QString err;
                if (!UdpTrackerProto::parseAnnounceResponse(dg, ex->txId, &interval, &peers, &err)) {
                    // Distinguish "not for us" (tx mismatch -> keep waiting) from a
                    // real error action (give up this exchange).
                    if (err == QStringLiteral("transaction id mismatch")) continue;
                    ex->timer->stop();
                    emit announceFailed(err);
                    cleanup();
                    return;
                }
                ex->timer->stop();
                emit peersReceived(peers, interval, 0); // UDP has no "min interval" field
                cleanup();
                return;
            }
        }
    });

    // Bind to any local port so we can receive replies, then kick off connect.
    if (!ex->sock->bind(QHostAddress::AnyIPv4, 0)) {
        emit announceFailed(QStringLiteral("udp bind failed"));
        cleanup();
        return;
    }
    sendConnect();
}
```

> Note on `host`: `m_tracker.host()` for `udp://tracker.example:9` is a hostname. `QUdpSocket::writeDatagram(QByteArray, QHostAddress, port)` needs an IP. For the MVP/tests we use numeric hosts (127.0.0.1). Real hostname resolution (`QHostInfo::lookupHost`) is a small follow-up recorded in the plan's Notes; if `QHostAddress(host)` is null, emit `announceFailed("unresolved udp tracker host")` before binding so it fails fast rather than sending to a null address.

Add that guard right before the `bind`:

```cpp
    if (QHostAddress(host).isNull()) {
        emit announceFailed(QStringLiteral("udp tracker host is not a numeric address (DNS is a follow-up): %1").arg(host));
        cleanup();
        return;
    }
```

- [ ] **Step 3: Create the in-process UDP tracker server**

Create `tests/TestUdpTracker.h`:

```cpp
#pragma once

#include "torrent/TrackerTypes.h"

#include <QObject>
#include <QVector>

class QUdpSocket;

// Minimal BEP 15 UDP tracker for offline tests. Answers connect (a fixed
// connection id) and announce (returns the configured peers + interval).
// Mirrors TestSeeder / TestFtpServer: in-process, event-loop, no threads.
class TestUdpTracker : public QObject {
    Q_OBJECT
public:
    explicit TestUdpTracker(QObject* parent = nullptr);
    quint16 port() const;
    void    setPeers(const QVector<PeerAddress>& peers) { m_peers = peers; }
    void    setInterval(int secs) { m_interval = secs; }

private:
    void onReadyRead();
    QUdpSocket*           m_sock = nullptr;
    QVector<PeerAddress>  m_peers;
    int                   m_interval = 1800;
    static constexpr quint64 kConnId = 0x0123456789ABCDEFULL;
};
```

Create `tests/TestUdpTracker.cpp`:

```cpp
#include "TestUdpTracker.h"

#include <QHostAddress>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>

TestUdpTracker::TestUdpTracker(QObject* parent) : QObject(parent), m_sock(new QUdpSocket(this)) {
    m_sock->bind(QHostAddress::LocalHost, 0);
    connect(m_sock, &QUdpSocket::readyRead, this, &TestUdpTracker::onReadyRead);
}

quint16 TestUdpTracker::port() const { return m_sock->localPort(); }

void TestUdpTracker::onReadyRead() {
    while (m_sock->hasPendingDatagrams()) {
        QNetworkDatagram in = m_sock->receiveDatagram();
        const QByteArray req = in.data();
        if (req.size() < 16) continue;
        const auto* p = reinterpret_cast<const uchar*>(req.constData());
        const quint32 action = qFromBigEndian<quint32>(p + 8);
        const quint32 txId = qFromBigEndian<quint32>(p + 12);

        QByteArray resp;
        if (action == 0u) { // connect
            resp.resize(16);
            auto* q = reinterpret_cast<uchar*>(resp.data());
            qToBigEndian<quint32>(0u, q);
            qToBigEndian<quint32>(txId, q + 4);
            qToBigEndian<quint64>(kConnId, q + 8);
        } else if (action == 1u) { // announce
            resp.resize(20);
            auto* q = reinterpret_cast<uchar*>(resp.data());
            qToBigEndian<quint32>(1u, q);
            qToBigEndian<quint32>(txId, q + 4);
            qToBigEndian<quint32>(quint32(m_interval), q + 8);
            qToBigEndian<quint32>(0u, q + 12); // leechers
            qToBigEndian<quint32>(quint32(m_peers.size()), q + 16); // seeders
            for (const PeerAddress& peer : m_peers) {
                QByteArray rec(6, char(0));
                const auto parts = peer.host.split('.');
                for (int i = 0; i < 4 && i < parts.size(); ++i) rec[i] = char(parts[i].toInt());
                rec[4] = char((peer.port >> 8) & 0xFF);
                rec[5] = char(peer.port & 0xFF);
                resp += rec;
            }
        } else {
            continue;
        }
        m_sock->writeDatagram(resp, in.senderAddress(), in.senderPort());
    }
}
```

- [ ] **Step 4: Link the server into the test and run**

In `tests/CMakeLists.txt`, change the `tst_udptracker` executable line to include the server:

```cmake
add_executable(tst_udptracker tst_udptracker.cpp TestUdpTracker.cpp)
target_link_libraries(tst_udptracker PRIVATE orbitcore Qt6::Test Qt6::Network)
```

Run: `cmake --build build -j && ctest --test-dir build -R tst_udptracker --output-on-failure`
Expected: PASS (pure + 2 live slots).

- [ ] **Step 5: Run the gate**

Run: `ctest --test-dir build -R "tst_udptracker|tst_tracker|tst_torrent" --output-on-failure`
Expected: all PASS.

- [ ] **Step 6: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 6: AnnounceController — tiers, promotion, hungry fallback, aggregation

**Files:**
- Create: `src/core/torrent/AnnounceController.h`, `src/core/torrent/AnnounceController.cpp`
- Modify: `src/core/CMakeLists.txt` (add `torrent/AnnounceController.cpp`)
- Test: `tests/tst_announcecontroller.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - ```cpp
    class AnnounceController : public QObject {
        Q_OBJECT
    public:
        using ClientFactory = std::function<ITrackerClient*(const QUrl&, QObject* parent)>;
        AnnounceController(const QVector<QVector<QUrl>>& tiers, QNetworkAccessManager* nam,
                           quint32 rngSeed, QObject* parent = nullptr, ClientFactory factory = {});
        int  trackerCount() const;
        void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                      qint64 downloaded, qint64 left, TrackerEvent ev, bool hungry);
    signals:
        void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
        void announceFailed(QString reason);
    };
    ```
  - Behavior: non-hungry announce contacts the **front** tracker of each tier; on that front's `announceFailed`, it advances to the **next** tracker in the same tier (in-tier BEP 12 fallback) until one succeeds or the tier is exhausted. On success, that tracker is **promoted to the front** of its tier and its peers/interval are forwarded up. Hungry announce contacts **every** tracker in every tier at once and forwards each result. Forwarded `intervalSecs` is the min positive interval seen this round; `minIntervalSecs` is the max min-interval seen this round.
- Consumes: `ITrackerClient` (Task 3), `HttpTrackerClient` (Task 3), `UdpTrackerClient` (Task 5).

- [ ] **Step 1: Write the failing test (with a fake client)**

Create `tests/tst_announcecontroller.cpp`:

```cpp
#include "torrent/AnnounceController.h"
#include "torrent/ITrackerClient.h"

#include <QSignalSpy>
#include <QtTest>

// A scriptable fake tracker client: each announce() emits whatever the test
// queued for this URL (peers+interval, or a failure), on the next event-loop turn.
class FakeClient : public ITrackerClient {
    Q_OBJECT
public:
    FakeClient(const QUrl& url, QObject* parent) : ITrackerClient(parent), m_url(url) {}
    QUrl trackerUrl() const override { return m_url; }
    void announce(const QByteArray&, const QByteArray&, quint16, qint64, qint64, TrackerEvent) override {
        ++announces;
        QMetaObject::invokeMethod(this, [this] {
            if (m_fail) emit announceFailed(QStringLiteral("boom"));
            else emit peersReceived(m_peers, m_interval, m_minInterval);
        }, Qt::QueuedConnection);
    }
    QUrl m_url; QVector<PeerAddress> m_peers; int m_interval = 0; int m_minInterval = 0; bool m_fail = false;
    int announces = 0;
};

class TestAnnounceController : public QObject {
    Q_OBJECT
    // maps URL -> FakeClient so tests can script each one
    QHash<QString, FakeClient*> m_fakes;

    AnnounceController::ClientFactory factory() {
        return [this](const QUrl& u, QObject* parent) -> ITrackerClient* {
            auto* f = new FakeClient(u, parent);
            m_fakes.insert(u.toString(), f);
            return f;
        };
    }

private slots:
    void init() { m_fakes.clear(); }

    void nonHungry_frontOfEachTier_forwardsUnion() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a")}, {QUrl("http://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        // script both fronts
        auto* a = m_fakes_will("udp://a"); Q_UNUSED(a);
        AnnounceController* c = &ctl;
        QSignalSpy spy(c, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/false);
        m_fakes["udp://a"]->m_peers = {{QStringLiteral("1.1.1.1"), 1}};
        m_fakes["http://b"]->m_peers = {{QStringLiteral("2.2.2.2"), 2}};
        // (peers were set after construction; re-run to emit with data)
        // Instead, script BEFORE announce in real test — see note.
        QVERIFY(spy.wait(1000));
        QVERIFY(spy.count() >= 1);
    }

    void inTierFailover_advancesToNextTracker_andPromotes() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://dead"), QUrl("http://live")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        // Build clients eagerly so we can script them: trackerCount() forces creation.
        QCOMPARE(ctl.trackerCount(), 2);
        m_fakes["udp://dead"]->m_fail = true;
        m_fakes["http://live"]->m_peers = {{QStringLiteral("3.3.3.3"), 3}};
        QSignalSpy ok(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, false);
        QVERIFY(ok.wait(1000));
        QCOMPARE(m_fakes["udp://dead"]->announces, 1);
        QCOMPARE(m_fakes["http://live"]->announces, 1); // advanced to backup
        // Next round: live is now front, dead is not contacted.
        m_fakes["udp://dead"]->announces = 0;
        m_fakes["http://live"]->announces = 0;
        QSignalSpy ok2(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, false);
        QVERIFY(ok2.wait(1000));
        QCOMPARE(m_fakes["http://live"]->announces, 1);
        QCOMPARE(m_fakes["udp://dead"]->announces, 0); // promoted 'live' is now front
    }

    void hungry_contactsEveryTracker() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a"), QUrl("http://a2")}, {QUrl("udp://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 3);
        for (auto* f : m_fakes) f->m_peers = {{QStringLiteral("4.4.4.4"), 4}};
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/true);
        QTRY_COMPARE(m_fakes["udp://a"]->announces, 1);
        QCOMPARE(m_fakes["http://a2"]->announces, 1);
        QCOMPARE(m_fakes["udp://b"]->announces, 1);
    }

    void allFail_emitsAnnounceFailed() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://x")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 1);
        m_fakes["udp://x"]->m_fail = true;
        QSignalSpy bad(&ctl, &AnnounceController::announceFailed);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, false);
        QVERIFY(bad.wait(1000));
    }

private:
    FakeClient* m_fakes_will(const QString&) { return nullptr; } // placeholder, see note
};

QTEST_MAIN(TestAnnounceController)
#include "tst_announcecontroller.moc"
```

> Implementer note: `trackerCount()` must eagerly create every tier's clients via the factory (so tests can script them before `announce`). Delete the illustrative `nonHungry_frontOfEachTier_forwardsUnion` scaffolding if it fights the "script before announce" ordering — the **contract-bearing** tests are `inTierFailover_advancesToNextTracker_andPromotes`, `hungry_contactsEveryTracker`, and `allFail_emitsAnnounceFailed`. Keep those; make the union test script peers **before** calling `announce`.

- [ ] **Step 2: Run to verify it fails**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_announcecontroller tst_announcecontroller.cpp)
target_link_libraries(tst_announcecontroller PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_announcecontroller COMMAND tst_announcecontroller)
```

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build -j 2>&1 | head -20`
Expected: FAIL — `AnnounceController.h` not found.

- [ ] **Step 3: Implement the header**

Create `src/core/torrent/AnnounceController.h`:

```cpp
#pragma once

#include "torrent/ITrackerClient.h"
#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

// Owns the announce tiers (BEP 12) and every ITrackerClient, and presents
// TorrentTask the same two signals a single tracker did. It routes WHICH
// trackers to contact; TorrentTask still owns WHEN (the announce timer/cadence).
class AnnounceController : public QObject {
    Q_OBJECT
public:
    using ClientFactory = std::function<ITrackerClient*(const QUrl&, QObject* parent)>;

    AnnounceController(const QVector<QVector<QUrl>>& tiers, QNetworkAccessManager* nam,
                       quint32 rngSeed, QObject* parent = nullptr, ClientFactory factory = {});

    int trackerCount() const;

    // hungry=false: BEP 12 — front of each tier, advancing to the next tracker
    // in a tier on failure, promoting the responder to front. hungry=true:
    // contact every tracker in every tier this round.
    void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                  qint64 downloaded, qint64 left, TrackerEvent ev, bool hungry);

signals:
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);

private:
    struct Tier {
        QVector<ITrackerClient*> clients; // index 0 = current front
        int attempt = 0;                  // in-tier fallback cursor for the active round
    };

    ITrackerClient* makeClient(const QUrl& url);
    void onClientPeers(int tierIdx, ITrackerClient* c, const QVector<PeerAddress>& peers,
                       int interval, int minInterval);
    void onClientFailed(int tierIdx, ITrackerClient* c, const QString& why);
    void tryTierAt(int tierIdx, int idx); // non-hungry: contact clients[idx], fall forward on fail

    QNetworkAccessManager* m_nam;
    quint32                m_rngSeed;
    ClientFactory          m_factory;
    QVector<Tier>          m_tiers;

    // Per-round aggregation + book-keeping (reset at the top of announce()).
    QByteArray m_infoHash, m_peerId;
    quint16    m_port = 0;
    qint64     m_downloaded = 0, m_left = 0;
    TrackerEvent m_ev = TrackerEvent::None;
    int  m_roundInterval = 0;      // min positive interval seen
    int  m_roundMinInterval = 0;   // max min-interval seen
    int  m_tiersFailed = 0;        // how many tiers exhausted with no success this round
};
```

- [ ] **Step 4: Implement the controller**

Create `src/core/torrent/AnnounceController.cpp`:

```cpp
#include "torrent/AnnounceController.h"

#include "torrent/HttpTrackerClient.h"
#include "torrent/UdpTrackerClient.h"

#include <QNetworkAccessManager>

AnnounceController::AnnounceController(const QVector<QVector<QUrl>>& tiers, QNetworkAccessManager* nam,
                                       quint32 rngSeed, QObject* parent, ClientFactory factory)
    : QObject(parent), m_nam(nam), m_rngSeed(rngSeed), m_factory(std::move(factory)) {
    // Eagerly build every client so behavior (and tests) can rely on them
    // existing up-front; ordering within a tier is the announce-list order.
    for (int t = 0; t < tiers.size(); ++t) {
        Tier tier;
        for (const QUrl& url : tiers[t]) {
            ITrackerClient* c = makeClient(url);
            if (!c) continue;
            const int tierIdx = t;
            connect(c, &ITrackerClient::peersReceived, this,
                    [this, tierIdx, c](QVector<PeerAddress> peers, int interval, int minInterval) {
                        onClientPeers(tierIdx, c, peers, interval, minInterval);
                    });
            connect(c, &ITrackerClient::announceFailed, this,
                    [this, tierIdx, c](const QString& why) { onClientFailed(tierIdx, c, why); });
            tier.clients.append(c);
        }
        if (!tier.clients.isEmpty()) m_tiers.append(tier);
    }
}

ITrackerClient* AnnounceController::makeClient(const QUrl& url) {
    if (m_factory) return m_factory(url, this);
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return m_nam ? new HttpTrackerClient(m_nam, url, this) : nullptr;
    if (scheme == QLatin1String("udp"))
        return new UdpTrackerClient(url, m_rngSeed, this);
    return nullptr; // unknown scheme dropped
}

int AnnounceController::trackerCount() const {
    int n = 0;
    for (const Tier& t : m_tiers) n += t.clients.size();
    return n;
}

void AnnounceController::announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                                  qint64 downloaded, qint64 left, TrackerEvent ev, bool hungry) {
    m_infoHash = infoHash; m_peerId = peerId; m_port = port;
    m_downloaded = downloaded; m_left = left; m_ev = ev;
    m_roundInterval = 0; m_roundMinInterval = 0; m_tiersFailed = 0;

    if (m_tiers.isEmpty()) { emit announceFailed(QStringLiteral("no usable trackers")); return; }

    for (int t = 0; t < m_tiers.size(); ++t) {
        m_tiers[t].attempt = 0;
        if (hungry) {
            for (ITrackerClient* c : m_tiers[t].clients)
                c->announce(m_infoHash, m_peerId, m_port, m_downloaded, m_left, m_ev);
        } else {
            tryTierAt(t, 0);
        }
    }
}

void AnnounceController::tryTierAt(int tierIdx, int idx) {
    Tier& tier = m_tiers[tierIdx];
    if (idx < 0 || idx >= tier.clients.size()) { // tier exhausted
        if (++m_tiersFailed == m_tiers.size())
            emit announceFailed(QStringLiteral("all trackers failed"));
        return;
    }
    tier.attempt = idx;
    tier.clients[idx]->announce(m_infoHash, m_peerId, m_port, m_downloaded, m_left, m_ev);
}

void AnnounceController::onClientPeers(int tierIdx, ITrackerClient* c, const QVector<PeerAddress>& peers,
                                       int interval, int minInterval) {
    // Promote the responder to the front of its tier.
    Tier& tier = m_tiers[tierIdx];
    const int at = tier.clients.indexOf(c);
    if (at > 0) tier.clients.move(at, 0);

    if (interval > 0 && (m_roundInterval == 0 || interval < m_roundInterval)) m_roundInterval = interval;
    if (minInterval > m_roundMinInterval) m_roundMinInterval = minInterval;

    emit peersReceived(peers, m_roundInterval, m_roundMinInterval);
}

void AnnounceController::onClientFailed(int tierIdx, ITrackerClient* c, const QString& /*why*/) {
    Tier& tier = m_tiers[tierIdx];
    // Only advance if this failure is for the tier's current (non-hungry) attempt.
    if (tier.attempt < tier.clients.size() && tier.clients[tier.attempt] == c)
        tryTierAt(tierIdx, tier.attempt + 1);
}
```

Add `torrent/AnnounceController.cpp` to `src/core/CMakeLists.txt`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build -j && ctest --test-dir build -R tst_announcecontroller --output-on-failure`
Expected: PASS (contract-bearing slots).

- [ ] **Step 6: Run the gate**

Run: `ctest --test-dir build -R "tst_announcecontroller|tst_udptracker|tst_tracker|tst_torrent" --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 7: Wire AnnounceController into TorrentTask

**Files:**
- Modify: `src/core/torrent/TorrentTask.h` (replace `HttpTrackerClient* m_tracker` with `AnnounceController* m_announce`)
- Modify: `src/core/torrent/TorrentTask.cpp` (`beginLeeching`, the announce-timer tick, `maybeFinish`/`pause`/`cancel` announce calls)
- Test: `tests/tst_torrent.cpp` (a wiring assertion; existing E2E still green via `addPeerForTest`)

**Interfaces:**
- Consumes: `AnnounceController` (Task 6), `TorrentMetainfo::announceList` (Task 2).
- Produces: no new public API. `TorrentTask` now announces through `m_announce`.

- [ ] **Step 1: Update the header**

In `src/core/torrent/TorrentTask.h`:
- Replace `#include "torrent/HttpTrackerClient.h" // PeerAddress` with `#include "torrent/TrackerTypes.h"` and add a forward declaration `class AnnounceController;` near the other forward decls.
- Replace the member `HttpTrackerClient* m_tracker = nullptr;` with `AnnounceController* m_announce = nullptr;`.

- [ ] **Step 2: Rewrite the announce wiring in `beginLeeching`**

In `src/core/torrent/TorrentTask.cpp`, add `#include "torrent/AnnounceController.h"` at the top. Replace the whole `if (m_nam && !m_meta.announce.isEmpty()) { ... }` block in `beginLeeching()` (currently lines ~217–263) with the tier-driven version:

```cpp
    if (!m_meta.announceList.isEmpty()) {
        if (!m_announce) {
            m_announce = new AnnounceController(m_meta.announceList, m_nam, m_rngSeed, this);
            connect(m_announce, &AnnounceController::peersReceived, this,
                    [this](QVector<PeerAddress> peers, int interval, int minInterval) {
                        int newCount = 0;
                        for (const auto& p : peers) {
                            const QString k = peerKey(p);
                            if (m_knownPeers.contains(k)) continue;
                            m_knownPeers.insert(k);
                            m_pendingPeers.append(p);
                            ++newCount;
                        }
                        if (interval > 0) m_announceIntervalSecs = interval;
                        if (minInterval > 0) m_minAnnounceIntervalSecs = minInterval;
                        m_trackerStatus = QStringLiteral("OK: %1 peers (interval %2s)")
                                              .arg(peers.size())
                                              .arg(m_announceIntervalSecs > 0 ? m_announceIntervalSecs : 1800);
                        logLine(LogLevel::Info,
                                QStringLiteral("tracker: %1 peers received (%2 new), interval %3s")
                                    .arg(peers.size()).arg(newCount).arg(interval));
                        rescheduleAnnounceTimer();
                        openPeers();
                    });
            connect(m_announce, &AnnounceController::announceFailed, this,
                    [this](const QString& why) {
                        m_trackerStatus = QStringLiteral("failed: %1").arg(why);
                        logLine(LogLevel::Warn, QStringLiteral("announce failed: %1").arg(why));
                        rescheduleAnnounceTimer();
                    });
        }
        const qint64 left = m_totalWantedBytes - m_verifiedBytes;
        m_trackerStatus = QStringLiteral("announcing…");
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                             TrackerEvent::Started, /*hungry*/false);

        if (!m_announceTimer) {
            m_announceTimer = new QTimer(this);
            connect(m_announceTimer, &QTimer::timeout, this, [this] {
                if (!m_announce) return;
                const qint64 left = m_totalWantedBytes - m_verifiedBytes;
                // Hungry when peer-starved: widen to every tracker to refill fast.
                const bool hungry = (connectedPeerCount() == 0);
                m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                                     TrackerEvent::None, hungry);
            });
        }
        rescheduleAnnounceTimer();
    }
```

- [ ] **Step 3: Update the remaining announce call sites (completed/stopped)**

In `src/core/torrent/TorrentTask.cpp`, the three other announce sites (around the old lines 495, 513, 534 — `Completed` on finish, `Stopped` on pause, `Stopped` on cancel) currently read `if (m_tracker && m_nam && !m_meta.announce.isEmpty()) { m_tracker->announce(m_meta.announce, ...); }`. Replace each guard/call with the controller form, dropping the URL arg and adding `hungry=false`:

```cpp
    // completed (in maybeFinish):
    if (m_announce) {
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, 0,
                             TrackerEvent::Completed, false);
    }
```
```cpp
    // stopped (in pause() and cancel()):
    if (m_announce) {
        const qint64 left = m_totalWantedBytes - m_verifiedBytes;
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                             TrackerEvent::Stopped, false);
    }
```

Leave the surrounding `if (m_announceTimer) m_announceTimer->stop();` lines exactly as they are.

- [ ] **Step 4: Add a wiring test**

Add to `tests/tst_torrent.cpp` a slot that builds a `TorrentMetainfo` with a two-tier `announceList` and asserts the task starts without crashing and reaches `Connecting` (no real trackers reachable, but the controller must be constructed and announce attempted). Use the file's existing helpers for building a metainfo + task. Minimal shape:

```cpp
    void multiTracker_startsAndAnnouncesWithoutCrash() {
        TorrentMetainfo m = /* existing helper to build a small valid single-file metainfo */;
        m.announceList = {{QUrl("udp://127.0.0.1:1")}, {QUrl("http://127.0.0.1:1/annou")}};
        // ...construct TorrentTask as the other tests do (nam, resumeDir, etc.)...
        task.start();
        QTRY_VERIFY(task.state() == DownloadState::Connecting || task.state() == DownloadState::Checking);
        // No peers will arrive; the point is the controller path is exercised without crashing.
    }
```

- [ ] **Step 5: Build and run the torrent suite**

Run: `cmake --build build -j && ctest --test-dir build -R tst_torrent --output-on-failure`
Expected: PASS (existing E2E via `addPeerForTest` still green; new wiring slot green).

- [ ] **Step 6: Full gate**

Run: `ctest --test-dir build -R "tst_download|tst_ftp|tst_torrent|tst_metainfo|tst_tracker|tst_udptracker|tst_announcecontroller|tst_trackerpeers|tst_seeder" --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Confirm the GUI still links and boots headless**

Run: `cmake --build build -j --target orbit-gui && QT_QPA_PLATFORM=offscreen ./build/src/gui/orbit-gui --version 2>/dev/null; echo "exit=$?"`
Expected: builds; boots without crash (exact flag/output per how the repo's other checks boot it — the point is no link/runtime break from the tracker refactor).

- [ ] **Step 8: Stop for review** — do NOT commit. Report the diff and gate result.

---

## Task 8: Offline E2E — download through a UDP tracker

**Files:**
- Modify: `tests/tst_torrent.cpp` (E2E slot using `TestUdpTracker` + `TestSeeder`)
- Modify: `tests/CMakeLists.txt` (link `TestUdpTracker.cpp` into `tst_torrent`)

**Interfaces:**
- Consumes: `TestUdpTracker` (Task 5), `TestSeeder` (existing), the full tracker path (Tasks 1–7).

- [ ] **Step 1: Write the E2E test**

Add to `tests/tst_torrent.cpp` (add `#include "TestUdpTracker.h"`):

```cpp
    void e2e_downloadsThroughUdpTracker() {
        // 1) Stand up the in-process seeder for a known small multi-block torrent
        //    (reuse the exact fixture the existing E2E uses — grep for the helper
        //    that builds the seeder + metainfo, e.g. makeSeededTorrent()).
        TestSeeder seeder;
        TorrentMetainfo meta = /* existing helper: metainfo whose pieces the seeder serves */;
        seeder.serve(meta, /*payload*/ knownPayload);
        const quint16 seederPort = seeder.port();

        // 2) Stand up the UDP tracker returning the seeder as the only peer.
        TestUdpTracker tracker;
        tracker.setPeers({{QStringLiteral("127.0.0.1"), seederPort}});

        // 3) Point the torrent's announce-list at the local UDP tracker ONLY.
        meta.announce = QUrl();
        meta.announceList = {{QUrl(QStringLiteral("udp://127.0.0.1:%1").arg(tracker.port()))}};

        // 4) Run a real TorrentTask (no addPeerForTest — peers must come from the tracker).
        const QString dir = /* temp dir as other tests make it */;
        TorrentTask task(meta, dir, /*selected*/ allFiles(meta), PieceStrategy::Sequential,
                         /*listenPort*/ 6881, /*maxPeers*/ 50, ResumeVerifyMode::TrustBitfield,
                         /*rngSeed*/ 42u, &nam, nullptr, nullptr, dir);
        QSignalSpy done(&task, &AbstractTask::stateChanged);
        task.start();

        // 5) Expect completion and byte-identical payload.
        QTRY_VERIFY_WITH_TIMEOUT(task.state() == DownloadState::Completed, 15000);
        QByteArray got = /* read <dir>/<name> */;
        QCOMPARE(got, knownPayload);
    }
```

> Implementer note: mirror the **existing** E2E slot in `tst_torrent.cpp` for fixture/seeder/temp-dir setup (payload bytes, piece length, `TestSeeder::serve` signature, how the file is read back). The only differences from the existing E2E are: (a) peers come from `TestUdpTracker` instead of `addPeerForTest`, and (b) `announceList` is a `udp://` local URL. Keep the multi-block piece size the existing E2E uses (≥256 KiB / >8 blocks per piece) so the picker's multi-block path stays covered.

- [ ] **Step 2: Link the UDP server into the torrent test**

In `tests/CMakeLists.txt`, update the `tst_torrent` executable to include `TestUdpTracker.cpp`:

```cmake
add_executable(tst_torrent tst_torrent.cpp TestSeeder.cpp TestUdpTracker.cpp)
```
(keep the existing `target_link_libraries` line for `tst_torrent`.)

- [ ] **Step 3: Run the E2E**

Run: `cmake --build build -j && ctest --test-dir build -R tst_torrent --output-on-failure`
Expected: PASS, including `e2e_downloadsThroughUdpTracker`.

- [ ] **Step 4: Loop the E2E to check for flakiness**

Run: `for i in $(seq 1 20); do ctest --test-dir build -R "tst_torrent" --output-on-failure -Q || { echo "FLAKE at $i"; break; }; done; echo done`
Expected: 20/20 clean (the async connect→announce→download path must be stable, per the Sub-phase A flake lesson).

- [ ] **Step 5: Full gate + GUI boot**

Run: `ctest --test-dir build --output-on-failure`
Expected: entire suite PASS.

- [ ] **Step 6: Stop for review** — do NOT commit. Report the diff, the 20× loop result, and the full-suite result.

---

## Notes / deferred (record in the ROADMAP when landing)

- **DNS for UDP trackers.** `UdpTrackerClient` currently requires a numeric host (works for tests and IP-literal trackers). Real hostname trackers need `QHostInfo::lookupHost` before the first datagram — small follow-up; today it fails fast with a clear message.
- **Scrape (BEP 48 / BEP 15)** — deferred per the spec; bring it in when the Properties panel warrants seeders/leechers counts.
- **Interval aggregation is one global cadence** for all tiers (min interval / max min-interval), matching TorrentTask's single announce timer. Safe; a very fast tier may be announced to less often than it allows.
- **BE32/BE64 helpers** now exist in both `PeerWire` and `UdpTrackerProto` via `QtEndian` — acceptable duplication; a shared `torrent/ByteOrder.h` is a cosmetic follow-up.
- **RateLimiter** still does not cover torrent traffic (Sub-phase D), unchanged by this work.

---

## Self-Review

- **Spec coverage:** §3.1 ITrackerClient → Task 3. §3.2 TrackerPeers (IPv6+bogon) → Task 1. §3.3 announce-list → Task 2. §3.4 UdpTrackerClient (codecs, determinism, capped retries) → Tasks 4–5. §3.5 AnnounceController (BEP 12 order/promotion, hungry, aggregation, factory) → Task 6. §4 timer stays in TorrentTask → Task 7. §5 data flow unchanged → Task 7. §6 error/edge (silent tracker, connId expiry, garbage, all-fail, no-trackers) → Tasks 4–6 tests. §7 testing (pure units, in-process UDP server, offline E2E, gate) → Tasks 1–8. §8 scope/deferrals → Notes. All spec sections map to a task.
- **connection_id 60s TTL / re-connect (§3.4):** the live `UdpTrackerClient` re-connects per announce (fresh connect→announce each call), which satisfies "never use a stale connection id" more strongly than caching — noted here so a reviewer doesn't read it as a gap.
- **Placeholder scan:** no TBD/TODO; every code step carries complete code. The two "use the existing helper" notes (metainfo fixture in Task 2, seeder/E2E fixture in Task 8) are deliberate references to real repo helpers with the assertions/contract spelled out — not placeholders for logic.
- **Type consistency:** `PeerAddress`/`TrackerEvent` (TrackerTypes.h) used identically across all tasks; `announce(...)` signature is uniform on `ITrackerClient`, `HttpTrackerClient`, `UdpTrackerClient`, and forwarded by `AnnounceController::announce(..., bool hungry)`; `AnnounceController::ClientFactory` matches its use in Task 6's test.

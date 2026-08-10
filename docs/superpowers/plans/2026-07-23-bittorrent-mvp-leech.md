# BitTorrent MVP (leech-only) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a from-scratch, download-only BitTorrent client to `orbitcore` that downloads a `.torrent` end-to-end through an HTTP-tracker swarm, with SHA-1 verification, selective files, crash-safe resume, bandwidth cap, and the existing GUI (row, grid, context menu, Preferences).

**Architecture:** New pure units under `src/core/torrent/` (bencode, metainfo, bitfield, piece store, piece picker) carry the logic and the bulk of tests; thin Qt units (`HttpTrackerClient`, `PeerConnection`, `TorrentTask`) drive the network. A new `AbstractTask` base lets `TorrentTask` reuse the existing manager/model/grid/menu by emitting the same signals as `DownloadTask`. Offline tests use a `QHttpServer` tracker double and a hand-written `TestSeeder`.

**Tech Stack:** C++20, Qt 6.11 (Core + Network only in `orbitcore`; Widgets in `orbitgui`), CMake, QtTest. No new third-party dependency (no libtorrent, no Boost).

## Global Constraints

- **C++20**, Qt 6.11 (Homebrew, `/opt/homebrew`). Configure: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`. Tests: `ctest --test-dir build --output-on-failure`.
- **`orbitcore` must not depend on QtWidgets** — Core + Network only (torrent code lives here and stays headless-testable).
- **No new third-party dependency.** BitTorrent is implemented from scratch over `QTcpSocket`/`QNetworkAccessManager`.
- **Download-only:** never send `unchoke`/`piece`; never accept incoming connections. `am_choking` is always true.
- **MVP announce is HTTP(S) primary `announce` only.** UDP or `announce-list` are out — a UDP/failed announce yields a clear `Error`.
- **Block size = 16 KiB (`1 << 14`).** Default pipeline depth = 8; default max peers/torrent = 50; default nominal listen port = 6881.
- **No `Math::random`/`QRandomGenerator::global`/wall-clock in pure headless logic** — inject seeds/time so tests are deterministic (project convention).
- **Info-hash = SHA-1 over the raw bytes of the `info` dict as they appear in the file** — never re-encode.
- **DO NOT COMMIT.** Project rule #1: never create a commit (not even a local one) without the maintainer's explicit authorization. The "Commit" steps below are **disabled** — instead, leave changes **unstaged/uncommitted** in the working tree for the maintainer to review and commit himself. When a commit is eventually authorized, messages are English Conventional Commits (`feat:`/`fix:`/`test:`…) with **no `Co-Authored-By` trailer**.
- **Gate:** the existing `tst_download` expectations must remain unchanged after the `AbstractTask` refactor.
- Spec: `docs/superpowers/specs/2026-07-23-bittorrent-mvp-leech-design.md`.

---

### Task 1: Bencode codec (pure)

**Files:**
- Create: `src/core/torrent/Bencode.h`, `src/core/torrent/Bencode.cpp`
- Modify: `src/core/CMakeLists.txt` (add sources), `tests/CMakeLists.txt` (add `tst_bencode`)
- Test: `tests/tst_bencode.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Bencode.h
  class BencodeValue {
  public:
      enum class Type { Int, Bytes, List, Dict };
      Type type() const;
      qint64 toInt() const;
      QByteArray toBytes() const;
      const QList<BencodeValue>& toList() const;
      const QMap<QByteArray, BencodeValue>& toDict() const;   // key-sorted
      bool contains(const QByteArray& key) const;
      const BencodeValue& operator[](const QByteArray& key) const;
      // Raw source span of THIS value within the parsed buffer (for info-hash):
      int rawBegin() const;   // byte offset in the original buffer
      int rawEnd() const;     // one-past-end
  };
  namespace Bencode {
      // ok=false on malformed input; errorPos set to the byte offset.
      BencodeValue decode(const QByteArray& in, bool* ok, QString* err = nullptr);
      QByteArray   encode(const BencodeValue& v);
  }
  ```
- Consumes: nothing.

- [ ] **Step 1: Write the failing tests** (`tests/tst_bencode.cpp`)

```cpp
#include <QtTest>
#include "torrent/Bencode.h"

class TstBencode : public QObject { Q_OBJECT
private slots:
    void decodesInt() {
        bool ok=false; auto v = Bencode::decode("i42e", &ok);
        QVERIFY(ok); QCOMPARE(v.type(), BencodeValue::Type::Int); QCOMPARE(v.toInt(), 42LL);
    }
    void decodesNegativeInt() {
        bool ok=false; auto v = Bencode::decode("i-7e", &ok);
        QVERIFY(ok); QCOMPARE(v.toInt(), -7LL);
    }
    void rejectsLeadingZeroAndNegZero() {
        bool ok=true; Bencode::decode("i03e", &ok); QVERIFY(!ok);
        ok=true; Bencode::decode("i-0e", &ok); QVERIFY(!ok);
    }
    void decodesByteString() {
        bool ok=false; auto v = Bencode::decode("4:spam", &ok);
        QVERIFY(ok); QCOMPARE(v.toBytes(), QByteArray("spam"));
    }
    void decodesListAndDict() {
        bool ok=false; auto v = Bencode::decode("d3:cow3:moo4:spam4:eggse", &ok);
        QVERIFY(ok); QCOMPARE(v.type(), BencodeValue::Type::Dict);
        QCOMPARE(v[QByteArray("cow")].toBytes(), QByteArray("moo"));
    }
    void rejectsTruncated() { bool ok=true; Bencode::decode("i42", &ok); QVERIFY(!ok); }
    void roundTrips() {
        QByteArray in = "d1:ai1e1:bl3:foo3:baree";
        bool ok=false; auto v = Bencode::decode(in, &ok); QVERIFY(ok);
        QCOMPARE(Bencode::encode(v), in);   // canonical (sorted keys) matches this canonical input
    }
    void exposesRawSpanOfNestedValue() {
        QByteArray in = "d4:infod1:ni1eee";
        bool ok=false; auto v = Bencode::decode(in, &ok); QVERIFY(ok);
        const BencodeValue& info = v[QByteArray("info")];
        QCOMPARE(in.mid(info.rawBegin(), info.rawEnd()-info.rawBegin()), QByteArray("d1:ni1ee"));
    }
};
QTEST_MAIN(TstBencode)
#include "tst_bencode.moc"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target tst_bencode` — Expected: FAIL to compile (`torrent/Bencode.h` missing).

- [ ] **Step 3: Implement `Bencode`**

Recursive-descent decoder over `const char*` cursor. Record `rawBegin` before parsing each value and `rawEnd` after. Enforce: `i…e` with no leading zero (except `i0e`), no `-0`; `<len>:<bytes>` with bounds check; dict keys are byte strings, store in a `QMap` (auto-sorted) — for `encode`, emit keys in sorted order (canonical). Set `*ok=false` and `*err`/`errorPos` on any bounds/format violation. `encode`: `Int→i%de`, `Bytes→%d:…`, `List→l…e`, `Dict→d…e` sorted.

- [ ] **Step 4: Wire CMake and run**

Add `torrent/Bencode.cpp` to `orbitcore` sources; add a `tst_bencode` target mirroring existing `tst_*` (link `orbitcore`, `Qt6::Test`). Run: `ctest --test-dir build -R tst_bencode --output-on-failure` — Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/torrent/Bencode.* src/core/CMakeLists.txt tests/tst_bencode.cpp tests/CMakeLists.txt
git commit -m "feat(core): add bencode codec with raw info-span for info-hash"
```

---

### Task 2: TorrentMetainfo parsing (pure)

**Files:**
- Create: `src/core/torrent/TorrentMetainfo.h`, `src/core/torrent/TorrentMetainfo.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_metainfo.cpp`

**Interfaces:**
- Consumes: `Bencode::decode`, `BencodeValue::rawBegin/rawEnd`.
- Produces:
  ```cpp
  struct FileEntry { QString path; qint64 length; qint64 offset; };
  struct TorrentMetainfo {
      QByteArray infoHash;              // 20-byte SHA-1 of raw info dict
      QString name;
      qint64 pieceLength = 0;
      QVector<QByteArray> pieceHashes;  // 20 bytes each
      qint64 totalLength = 0;
      QVector<FileEntry> files;         // >=1; offset is cumulative into the logical stream
      QUrl announce;
      bool isMultiFile = false;
      static TorrentMetainfo parse(const QByteArray& torrentBytes, bool* ok, QString* err = nullptr);
  };
  ```

- [ ] **Step 1: Write the failing tests** (`tests/tst_metainfo.cpp`)

Build the metainfo bytes in-test with `Bencode::encode` so the fixture is self-contained. Cover single-file and multi-file, and a known info-hash.

```cpp
#include <QtTest>
#include <QCryptographicHash>
#include "torrent/TorrentMetainfo.h"
#include "torrent/Bencode.h"

// helper: build a minimal single-file .torrent with the given piece hashes
static QByteArray singleFile(qint64 len, qint64 pieceLen, const QByteArray& pieces) {
    // d8:announce<..>4:infod6:lengthI..e4:name4:file12:piece lengthI..e6:pieces<..>ee
    // Construct via BencodeValue for exactness (see helper in impl notes).
    // ... returns bytes; also compute expected infoHash = SHA1(raw info dict).
}

class TstMetainfo : public QObject { Q_OBJECT
private slots:
    void parsesSingleFile() {
        QByteArray pieces(20, '\x11'); // one piece hash
        auto bytes = singleFile(/*len*/100, /*pieceLen*/100, pieces);
        bool ok=false; auto m = TorrentMetainfo::parse(bytes, &ok);
        QVERIFY(ok);
        QCOMPARE(m.isMultiFile, false);
        QCOMPARE(m.totalLength, 100LL);
        QCOMPARE(m.pieceHashes.size(), 1);
        QCOMPARE(m.files.size(), 1);
        QCOMPARE(m.files[0].offset, 0LL);
    }
    void computesInfoHashFromRawBytes() {
        QByteArray pieces(20, '\x11');
        auto bytes = singleFile(100, 100, pieces);
        bool ok=false; auto m = TorrentMetainfo::parse(bytes, &ok);
        // recompute expected: SHA1 over the exact info dict slice
        bool ok2=false; auto root = Bencode::decode(bytes, &ok2);
        const auto& info = root[QByteArray("info")];
        auto expect = QCryptographicHash::hash(bytes.mid(info.rawBegin(), info.rawEnd()-info.rawBegin()),
                                               QCryptographicHash::Sha1);
        QCOMPARE(m.infoHash, expect);
    }
    void parsesMultiFileOffsets() {
        // two files of 30 and 70; assert files[1].offset == 30, totalLength == 100
        // ... build multi-file fixture, parse, assert
    }
    void rejectsBadPiecesLength() {
        QByteArray pieces(19, '\x00'); // not a multiple of 20
        auto bytes = singleFile(100, 100, pieces);
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
};
QTEST_MAIN(TstMetainfo)
#include "tst_metainfo.moc"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target tst_metainfo` — Expected: FAIL to compile.

- [ ] **Step 3: Implement `TorrentMetainfo::parse`**

Decode; require `info` dict. `infoHash = SHA1(torrentBytes.mid(info.rawBegin(), span))`. Read `piece length`, split `pieces` into 20-byte hashes (error if `pieces.size() % 20 != 0`). Single-file: `length` + `name` → one `FileEntry{name,length,0}`, `totalLength=length`. Multi-file: `files` list of `{length, path:[segments]}` → build `FileEntry{join(path,'/'), length, runningOffset}`, accumulate `totalLength`. `announce` → `QUrl`. Validate `sum(files.length)` consistent with piece count (`ceil(total/pieceLen) == pieceHashes.size()`).

- [ ] **Step 4: Wire CMake and run** — Expected: `ctest -R tst_metainfo` PASS.

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(core): parse .torrent metainfo (single/multi-file, info-hash)"
```

---

### Task 3: Bitfield (pure)

**Files:**
- Create: `src/core/torrent/Bitfield.h`, `src/core/torrent/Bitfield.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_bitfield.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class Bitfield {
  public:
      explicit Bitfield(int pieceCount = 0);
      int  size() const;                 // piece count
      bool has(int i) const;
      void set(int i);
      int  count() const;                // pieces present
      bool isComplete() const;
      QByteArray toBytes() const;                       // bit 0 = MSB of byte 0; trailing bits 0
      static Bitfield fromBytes(const QByteArray& b, int pieceCount);
  };
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
#include <QtTest>
#include "torrent/Bitfield.h"
class TstBitfield : public QObject { Q_OBJECT
private slots:
    void setsAndCounts() { Bitfield bf(10); QVERIFY(!bf.has(3)); bf.set(3);
        QVERIFY(bf.has(3)); QCOMPARE(bf.count(),1); QVERIFY(!bf.isComplete()); }
    void wireBitOrderMsbFirst() { Bitfield bf(8); bf.set(0);
        QCOMPARE(bf.toBytes(), QByteArray(1, char(0x80))); }   // piece 0 -> MSB
    void roundTripsBytes() { Bitfield bf(12); bf.set(0); bf.set(11);
        auto b = bf.toBytes(); auto bf2 = Bitfield::fromBytes(b, 12);
        QVERIFY(bf2.has(0)); QVERIFY(bf2.has(11)); QVERIFY(!bf2.has(5)); }
    void completeWhenAllSet() { Bitfield bf(3); bf.set(0); bf.set(1); bf.set(2);
        QVERIFY(bf.isComplete()); }
};
QTEST_MAIN(TstBitfield)
#include "tst_bitfield.moc"
```

- [ ] **Step 2: Run to verify it fails** — `cmake --build build --target tst_bitfield` → FAIL.

- [ ] **Step 3: Implement `Bitfield`** — back with `QBitArray` or `std::vector<uint8_t>`. `toBytes`: `ceil(size/8)` bytes, piece `i` → byte `i/8`, bit `7-(i%8)`. `fromBytes`: inverse, ignore trailing pad bits.

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_bitfield` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add Bitfield with wire bit-order"`.

---

### Task 4: PieceStore — mapping, selection, verification

**Files:**
- Create: `src/core/torrent/PieceStore.h`, `src/core/torrent/PieceStore.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_piecestore.cpp`

**Interfaces:**
- Consumes: `TorrentMetainfo`, `FileEntry`.
- Produces:
  ```cpp
  struct WriteRegion { const FileEntry* file; qint64 fileOffset; qint64 length; qint64 bufOffset; };
  class PieceStore {
  public:
      PieceStore(const TorrentMetainfo& m, const QString& destDir, const QSet<int>& selectedFiles);
      // pure mapping — testable without touching disk:
      QVector<WriteRegion> regionsFor(int piece, qint64 begin, qint64 length) const;
      qint64 pieceSize(int piece) const;                 // last piece may be short
      bool    pieceIsWanted(int piece) const;            // overlaps any selected file
      QVector<int> wantedPieces() const;
      // disk I/O:
      bool  writePiece(int piece, const QByteArray& data, QString* err);   // creates/opens files lazily
      bool  verify(int piece, const QByteArray& data) const;               // SHA1 == metainfo hash
      bool  verifyOnDisk(int piece, QString* err) const;                   // read back + SHA1
      QString destRootPath() const;
  };
  ```

- [ ] **Step 1: Write the failing tests** (mapping + selection are pure; disk uses `QTemporaryDir`)

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "torrent/PieceStore.h"
#include "torrent/TorrentMetainfo.h"
// Build a 2-file metainfo in-test: fileA=30, fileB=70, pieceLength=25, total=100 (4 pieces).
class TstPieceStore : public QObject { Q_OBJECT
private slots:
    void mapsPieceSpanningTwoFiles() {
        auto m = twoFileMeta();            // fileA[0..30) fileB[30..100), pieceLen 25
        PieceStore s(m, "/tmp/x", {0,1});  // both selected
        auto r = s.regionsFor(1, 0, 25);   // piece 1 = bytes [25..50): 5 in fileA, 20 in fileB
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0].length, 5LL); QCOMPARE(r[1].length, 20LL);
    }
    void lastPieceIsShort() { auto m = twoFileMeta(); PieceStore s(m,"/tmp/x",{0,1});
        QCOMPARE(s.pieceSize(3), 25LL); }  // 100/25 exact here -> adjust fixture if needed
    void selectionMarksBoundaryPieceWanted() {
        auto m = twoFileMeta(); PieceStore s(m, "/tmp/x", {1}); // only fileB selected
        QVERIFY(s.pieceIsWanted(1));   // boundary piece overlaps fileB
        QVERIFY(!s.pieceIsWanted(0));  // piece 0 = [0..25) entirely in fileA (unselected)
    }
    void writeVerifyRoundTrip() {
        QTemporaryDir dir; auto m = oneFileMeta(/*len*/50,/*pieceLen*/50, /*dataKnown*/);
        PieceStore s(m, dir.path(), {0});
        QByteArray data = knownPieceBytes();
        QString e; QVERIFY(s.writePiece(0, data, &e));
        QVERIFY(s.verify(0, data));
        QVERIFY(s.verifyOnDisk(0, &e));
    }
};
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement `PieceStore`** — `regionsFor`: walk `files` whose `[offset, offset+length)` intersects the absolute range `[piece*pieceLength+begin, +length)`; emit `WriteRegion`s with clamped lengths and `bufOffset`. `pieceIsWanted`: any selected file intersects the piece's byte range. `writePiece`: for each region, lazily `QFile::open(ReadWrite)` (create parent dirs), `seek(fileOffset)`, `write(data.mid(bufOffset,length))`. `verify`: `SHA1(data)==pieceHashes[piece]`. `verifyOnDisk`: read each region back into a buffer then `verify`.

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_piecestore` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add PieceStore (multi-file mapping, selection, SHA1 verify)"`.

---

### Task 5: PiecePicker — both strategies (pure)

**Files:**
- Create: `src/core/torrent/PiecePicker.h`, `src/core/torrent/PiecePicker.cpp`
- Modify: `src/core/DownloadTypes.h` (add `enum class PieceStrategy { RarestFirst, Sequential };`), `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_piecepicker.cpp`

**Interfaces:**
- Consumes: `Bitfield`, `PieceStrategy`, block size.
- Produces:
  ```cpp
  struct BlockRequest { int piece; qint64 begin; qint64 length; };
  class PiecePicker {
  public:
      PiecePicker(int pieceCount, qint64 pieceLength, qint64 totalLength,
                  const QVector<int>& wantedPieces, PieceStrategy s, quint32 rngSeed);
      void setStrategy(PieceStrategy s);
      void setHave(const Bitfield& ours);            // pieces we already have
      void addPeerBitfield(const Bitfield& peer);    // folds into availability counts
      void peerHas(int piece);                       // single 'have'
      // pick up to `count` blocks the peer (with peerBits) can serve, not already in-flight:
      QVector<BlockRequest> pick(const Bitfield& peerBits, int count);
      void markInFlight(const BlockRequest& b);
      void clearInFlight(const BlockRequest& b);
      void markPieceComplete(int piece);
  };
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
#include <QtTest>
#include "torrent/PiecePicker.h"
#include "torrent/Bitfield.h"
class TstPiecePicker : public QObject { Q_OBJECT
private slots:
    void sequentialPicksLowestIndex() {
        PiecePicker p(4, 16384, 4*16384, {0,1,2,3}, PieceStrategy::Sequential, 1);
        Bitfield peer(4); peer.set(0); peer.set(2);
        auto b = p.pick(peer, 1); QCOMPARE(b.size(),1); QCOMPARE(b[0].piece, 0);
    }
    void rarestFirstPrefersLowAvailability() {
        PiecePicker p(4, 16384, 4*16384, {0,1,2,3}, PieceStrategy::RarestFirst, 1);
        Bitfield a(4); a.set(0); a.set(1); a.set(2); p.addPeerBitfield(a); // pieces 0,1,2 common
        Bitfield b(4); b.set(0); b.set(1);            p.addPeerBitfield(b);
        Bitfield c(4); c.set(3);                       p.addPeerBitfield(c); // piece 3 rare
        Bitfield peer(4); peer.set(1); peer.set(3);
        auto req = p.pick(peer, 1); QCOMPARE(req[0].piece, 3);  // rarest among what peer has
    }
    void doesNotPickInFlightOrHave() {
        PiecePicker p(2, 16384, 2*16384, {0,1}, PieceStrategy::Sequential, 1);
        Bitfield ours(2); ours.set(0); p.setHave(ours);
        Bitfield peer(2); peer.set(0); peer.set(1);
        auto b = p.pick(peer, 5);
        for (auto& r : b) QVERIFY(r.piece != 0);        // already have piece 0
    }
    void splitsPieceIntoBlocks() {
        PiecePicker p(1, 40000, 40000, {0}, PieceStrategy::Sequential, 1); // 40000 -> 16384+16384+7232
        Bitfield peer(1); peer.set(0);
        auto b = p.pick(peer, 10); QCOMPARE(b.size(), 3);
        QCOMPARE(b[2].length, qint64(40000-2*16384));
    }
};
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement `PiecePicker`** — availability = `QVector<int>` incremented per peer bitfield/`have`. Candidate pieces = wanted ∧ !have ∧ !fullyInFlight ∧ peerHas. Order: Sequential → ascending index; RarestFirst → ascending availability, ties broken by a deterministic shuffle from `rngSeed` (seed a `std::mt19937`). Emit blocks (16 KiB, last block short) skipping in-flight `(piece,begin)`; stop at `count`.

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_piecepicker` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add PiecePicker (rarest-first + sequential)"`.

---

### Task 6: `AbstractTask` base + `DownloadTask` refactor (behavior-preserving)

**Files:**
- Create: `src/core/AbstractTask.h`
- Modify: `src/core/DownloadTask.h`/`.cpp` (inherit + thin accessors), `src/core/DownloadTypes.h` (`DownloadState::Checking`, `enum class ResumeVerifyMode { TrustBitfield, RecheckOnOpen };`), `src/core/DownloadManager.h`/`.cpp` (`m_tasks` → `QVector<AbstractTask*>`), `src/gui/DownloadTableModel.cpp` (`Checking` text/colors; read via `AbstractTask`)
- Test: `tests/tst_download.cpp` (must stay green — the gate), add one `AbstractTask` polymorphism test

**Interfaces:**
- Produces: `AbstractTask` (exact interface from spec §1: `kind()`, `id()`, `state()`, `displayName()`, `totalBytes()`, `receivedBytes()`, `priority()`, `setPriority()`, `start()`, `pause()`, `requeue()`, `cancel()`, signals `progress`/`stateChanged`).
- Consumes: existing `DownloadTask` behavior.

- [ ] **Step 1: Write the failing test** (`tests/tst_download.cpp`, new case)

```cpp
void downloadTaskIsAnAbstractTask() {
    // construct a DownloadTask (as existing tests do) and use it via the base pointer
    AbstractTask* t = makeHttpTask();       // existing helper returns DownloadTask*
    QCOMPARE(t->kind(), AbstractTask::Kind::Http);
    QVERIFY(!t->id().isNull());
    QCOMPARE(t->state(), DownloadState::Queued);
}
```

- [ ] **Step 2: Run to verify it fails** — Expected: FAIL to compile (`AbstractTask` missing / `kind()` not found).

- [ ] **Step 3: Implement the refactor** — Add `AbstractTask.h` (pure virtual interface, `QObject`). Make `class DownloadTask : public AbstractTask`; move the `progress`/`stateChanged` signal declarations to the base (keep `segmentProgress` on `DownloadTask`); implement `kind()` (Http/Ftp by transport scheme), `displayName()` (basename of destPath), `totalBytes()`/`receivedBytes()` (thin wrappers over existing members). Change `DownloadManager::m_tasks` to `QVector<AbstractTask*>` and adjust `taskById`, `wire()`, `pump()`, `pause/resume/cancel/setPriority/remove` to the base API; where segment-specific code is needed, `qobject_cast<DownloadTask*>`. Add `DownloadState::Checking` and give `DownloadTableModel` text ("Checking") + a color; treat as active in `CategoryFilterProxy`.

- [ ] **Step 4: Run the full gate**

Run: `ctest --test-dir build -R "tst_download|tst_gui|tst_transport|tst_ftp" --output-on-failure`
Expected: PASS with **no change to `tst_download` expectations** (27 cases) — behavior preserved.

- [ ] **Step 5: Commit** — `git commit -am "refactor(core): extract AbstractTask base; add Checking state"`.

---

### Task 7: HttpTrackerClient

**Files:**
- Create: `src/core/torrent/HttpTrackerClient.h`/`.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_tracker.cpp`

**Interfaces:**
- Consumes: `Bencode`, `TorrentMetainfo` (announce URL, infoHash).
- Produces:
  ```cpp
  struct PeerAddress { QString host; quint16 port; };
  enum class TrackerEvent { None, Started, Stopped, Completed };
  class HttpTrackerClient : public QObject { Q_OBJECT
  public:
      HttpTrackerClient(QNetworkAccessManager* nam, QObject* parent=nullptr);
      void announce(const QUrl& tracker, const QByteArray& infoHash, const QByteArray& peerId,
                    quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev);
  signals:
      void peersReceived(QVector<PeerAddress> peers, int intervalSecs);
      void announceFailed(QString reason);
  };
  // pure helper, separately tested:
  namespace TrackerProto {
      QUrl buildAnnounceUrl(const QUrl& base, const QByteArray& infoHash, const QByteArray& peerId,
                            quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev);
      bool parseResponse(const QByteArray& body, QVector<PeerAddress>* peers, int* interval, QString* failure);
  }
  ```

- [ ] **Step 1: Write the failing tests** (pure builder/parser + live against `QHttpServer`)

```cpp
#include <QtTest>
#include <QHttpServer>
#include "torrent/HttpTrackerClient.h"
class TstTracker : public QObject { Q_OBJECT
private slots:
    void encodesInfoHashRaw() {
        QByteArray ih(20, '\x00'); ih[0]=char(0xAB);
        auto url = TrackerProto::buildAnnounceUrl(QUrl("http://t/announce"), ih, "-OB0001-abcdefghij", 6881, 0, 100, TrackerEvent::Started);
        QVERIFY(url.toEncoded().contains("info_hash=%AB%00")); // raw byte URL-encoding
        QVERIFY(url.toEncoded().contains("event=started"));
        QVERIFY(url.toEncoded().contains("compact=1"));
    }
    void parsesCompactPeers() {
        // peers = 2 * 6 bytes: 127.0.0.1:6881 and 10.0.0.5:80
        QByteArray body = Bencode::encode(/* d8:intervali1800e5:peers12:<bytes>e */);
        QVector<PeerAddress> peers; int iv=0; QString fail;
        QVERIFY(TrackerProto::parseResponse(body, &peers, &iv, &fail));
        QCOMPARE(peers.size(), 2);
        QCOMPARE(peers[0].host, QString("127.0.0.1")); QCOMPARE(peers[0].port, quint16(6881));
        QCOMPARE(iv, 1800);
    }
    void reportsFailureReason() {
        auto body = Bencode::encode(/* d14:failure reason9:no dice.e */);
        QVector<PeerAddress> peers; int iv=0; QString fail;
        QVERIFY(!TrackerProto::parseResponse(body, &peers, &iv, &fail));
        QCOMPARE(fail, QString("no dice."));
    }
    void announcesAgainstLocalServer() {
        QHttpServer srv; /* route /announce -> canned compact response */
        // start on an ephemeral port, point client at it, spin event loop, assert peersReceived fires
    }
};
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement** — `buildAnnounceUrl`: percent-encode `infoHash`/`peerId` **byte-wise** (use `QByteArray::toPercentEncoding`), append `port/uploaded=0/downloaded/left/compact=1/event`. `announce`: GET via `QNetworkAccessManager`, on finish → `parseResponse`; parse compact (`peers` as 6-byte records) **and** dict-list forms; emit `peersReceived(peers, interval)` or `announceFailed`. If `tracker.scheme()=="udp"` → emit `announceFailed("tracker uses UDP — supported in a later version")` synchronously.

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_tracker` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add HTTP tracker client (compact/dict peers, events)"`.

---

### Task 8: PeerConnection — wire protocol (download-only)

**Files:**
- Create: `src/core/torrent/PeerConnection.h`/`.cpp`, `src/core/torrent/PeerWire.h`/`.cpp` (pure framing)
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_peerwire.cpp` (pure), and a socket handshake case in `tst_torrent` later

**Interfaces:**
- Consumes: `Bitfield`, `RateLimiter*`, block/handshake formats.
- Produces:
  ```cpp
  namespace PeerWire {
      QByteArray handshake(const QByteArray& infoHash, const QByteArray& peerId);
      // parse one message from a buffer; returns bytes consumed (0 = need more):
      struct Msg { int id; QByteArray payload; };   // id=-1 keep-alive
      int parseMessage(const QByteArray& buf, Msg* out);
      QByteArray interested();
      QByteArray request(int piece, qint64 begin, qint64 length);
  }
  class PeerConnection : public QObject { Q_OBJECT
  public:
      PeerConnection(const PeerAddress&, const QByteArray& infoHash, const QByteArray& peerId,
                     int pieceCount, RateLimiter* limiter, QObject* parent=nullptr);
      void connectToPeer();
      void sendInterested();
      void sendRequest(int piece, qint64 begin, qint64 length);
      const Bitfield& peerBitfield() const;
      bool amUnchoked() const;
  signals:
      void handshakeOk();
      void bitfieldReceived();
      void haveReceived(int piece);
      void unchoked(); void choked();
      void blockReceived(int piece, qint64 begin, QByteArray data);
      void disconnected(QString reason);
  };
  ```

- [ ] **Step 1: Write the failing tests** (`tests/tst_peerwire.cpp`, pure framing)

```cpp
#include <QtTest>
#include "torrent/PeerWire.h"
class TstPeerWire : public QObject { Q_OBJECT
private slots:
    void buildsHandshake() {
        QByteArray ih(20,'\x01'), id(20,'\x02');
        auto h = PeerWire::handshake(ih, id);
        QCOMPARE(h.size(), 68); QCOMPARE(h[0], char(19));
        QCOMPARE(h.mid(1,19), QByteArray("BitTorrent protocol"));
        QCOMPARE(h.mid(28,20), ih);
    }
    void parsesKeepAlive() { PeerWire::Msg m; int n = PeerWire::parseMessage(QByteArray(4,'\x00'), &m);
        QCOMPARE(n,4); QCOMPARE(m.id,-1); }
    void parsesUnchoke() { QByteArray b; b.append(QByteArray::fromHex("00000001")); b.append(char(1));
        PeerWire::Msg m; int n = PeerWire::parseMessage(b,&m); QCOMPARE(n,5); QCOMPARE(m.id,1); }
    void needsMoreOnPartial() { PeerWire::Msg m; QCOMPARE(PeerWire::parseMessage(QByteArray(2,'\x00'), &m), 0); }
    void buildsRequest() { auto r = PeerWire::request(1, 16384, 16384);
        QCOMPARE(r.size(), 17); QCOMPARE(r[4], char(6)); }
};
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement `PeerWire` then `PeerConnection`** — `PeerWire`: big-endian length prefixes; `parseMessage` returns 0 if fewer than `4+len` bytes buffered. `PeerConnection`: `QTcpSocket`; on connect send handshake; accumulate `readyRead` into a buffer; first consume the 68-byte handshake (validate infoHash) then loop `parseMessage`; dispatch: `bitfield(5)`→build `Bitfield`+emit; `have(4)`→emit; `unchoke(1)`/`choke(0)`→flags+emit; `piece(7)`→`blockReceived`; ignore `request`/`choke`-of-us (download-only, we never send data). Apply `RateLimiter` backpressure with `setReadBufferSize` as HTTP/FTP workers do. Never send `unchoke`/`piece`/`bitfield` (we advertise nothing).

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_peerwire` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add peer wire protocol + PeerConnection (download-only)"`.

---

### Task 9: TestSeeder (offline test infrastructure)

**Files:**
- Create: `tests/TestSeeder.h`/`.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: consumed by `tst_torrent` (Task 10); this task ships a self-check case `tst_seeder.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class TestSeeder : public QObject { Q_OBJECT
  public:
      // Serves `data` split into `pieceLength` pieces for `infoHash`. Listens on 127.0.0.1:port().
      TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength, QObject* parent=nullptr);
      quint16 port() const;
  };
  ```

- [ ] **Step 1: Write the failing self-check** (`tests/tst_seeder.cpp`)

```cpp
#include <QtTest>
#include <QTcpSocket>
#include "TestSeeder.h"
#include "torrent/PeerWire.h"
class TstSeeder : public QObject { Q_OBJECT
private slots:
    void completesHandshakeAndUnchokes() {
        QByteArray ih(20,'\x09'), data(50000,'Z');
        TestSeeder s(ih, data, 16384);
        QTcpSocket sock; sock.connectToHost("127.0.0.1", s.port());
        QVERIFY(sock.waitForConnected());
        sock.write(PeerWire::handshake(ih, QByteArray(20,'\x01')));
        QVERIFY(sock.waitForReadyRead());
        QByteArray in = sock.readAll();
        QVERIFY(in.size() >= 68);                 // seeder echoes handshake, then bitfield+unchoke
    }
};
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement `TestSeeder`** — `QTcpServer` on an ephemeral port. Per connection: read the 68-byte handshake, validate/echo it, send a full `bitfield` (all pieces present) then `unchoke`. On `request(piece,begin,len)` reply with a `piece` message carrying `data.mid(piece*pieceLength+begin, len)`. Honor `keep-alive`. (This is a *seeder* — it uploads; it is test-only, not shipped in the app.)

- [ ] **Step 4: Wire CMake and run** — `ctest -R tst_seeder` PASS.

- [ ] **Step 5: Commit** — `git commit -am "test(core): add in-process BitTorrent TestSeeder"`.

---

### Task 10: TorrentTask — orchestration, resume, E2E

**Files:**
- Create: `src/core/torrent/TorrentTask.h`/`.cpp`
- Modify: `src/core/DownloadTypes.h` (`enum class PieceState { Missing, InFlight, Have };`), `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_torrent.cpp` (E2E against `TestSeeder`)

**Interfaces:**
- Consumes: `TorrentMetainfo`, `PieceStore`, `PiecePicker`, `Bitfield`, `HttpTrackerClient`, `PeerConnection`, `RateLimiter*`, `Logger*`, `AbstractTask`.
- Produces:
  ```cpp
  class TorrentTask : public AbstractTask { Q_OBJECT
  public:
      TorrentTask(const TorrentMetainfo& m, const QString& destDir, const QSet<int>& selectedFiles,
                  PieceStrategy strategy, quint16 listenPort, int maxPeers,
                  ResumeVerifyMode verify, quint32 rngSeed,
                  QNetworkAccessManager* nam, RateLimiter* limiter, Logger* logger,
                  const QString& resumeDir, QObject* parent=nullptr);
      Kind kind() const override { return Kind::Torrent; }
      // ... AbstractTask overrides ...
      void setStrategy(PieceStrategy s);
      void forceRecheck();
      const TorrentMetainfo& metainfo() const;
      PieceState pieceState(int i) const;
      void restoreBitfield();          // read <resumeDir>/<infoHash>.bitfield if present
  signals:
      void pieceStateChanged(int piece, PieceState st);
  };
  ```

- [ ] **Step 1: Write the failing E2E tests**

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include "torrent/TorrentTask.h"
#include "TestSeeder.h"
class TstTorrent : public QObject { Q_OBJECT
private slots:
    void downloadsSingleFileFromSeeder() {
        QByteArray data = makeData(50000);
        auto m = metaForData(data, 16384);          // builds metainfo + infoHash
        TestSeeder seeder(m.infoHash, data, 16384);
        QTemporaryDir dir; QNetworkAccessManager nam; RateLimiter rl(0);
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        // Inject the seeder as a peer directly (bypass tracker) via a test seam:
        t.addPeerForTest({"127.0.0.1", seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state()==DownloadState::Completed, 10000);
        QCOMPARE(readFile(dir.filePath(m.name)), data);   // byte-identical
    }
    void resumesAfterRestart() {
        // start, wait for ~half the pieces, destroy the task (bitfield persisted),
        // construct a new TorrentTask over the same dir, start, assert it Completes
        // and downloads only the missing pieces (assert final bytes identical).
    }
    void forceRecheckRebuildsBitfield() {
        // complete a download, corrupt the bitfield to all-zero, forceRecheck(),
        // assert state returns to Completed without re-downloading (verifyOnDisk passes).
    }
    void downloadsSelectedFilesOnly() {
        // 2-file torrent, select only file B; assert file B bytes correct and
        // pieces entirely inside unselected file A are never requested.
    }
};
```

Add a tiny test seam `void addPeerForTest(const PeerAddress&)` (guarded, e.g. compiled always but only used by tests) so the E2E does not need a live tracker; tracker wiring is covered by `tst_tracker`.

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement `TorrentTask`** — Compose `PieceStore`, `PiecePicker`, `Bitfield`, `HttpTrackerClient`. State machine per spec §9: `start()` → optional `Checking` (`verifyOnDisk` for each wanted piece when `RecheckOnOpen` or `forceRecheck`) → `Connecting` (announce `Started` + `addPeerForTest`/tracker peers) → open up to `maxPeers` `PeerConnection`s → on `unchoked`, `pick()` blocks and `sendRequest` (pipeline depth 8) → accumulate blocks per piece → on full piece `PieceStore::verify` → on pass `writePiece` + `Bitfield::set` + `pieceStateChanged(Have)` + `markPieceComplete` + emit `progress` (≤1 Hz timer) → repeat → on `isComplete` announce `Completed`, state `Completed`. `pause()` announce `Stopped`, drop peers, keep bitfield, persist. Persist bitfield (debounced) to `<resumeDir>/<hexInfoHash>.bitfield` with the header from spec §12. On verify fail: discard piece, `clearInFlight`, re-request; drop a peer after repeated bad pieces. Feed `RateLimiter` through each `PeerConnection`.

- [ ] **Step 4: Run the E2E**

Run: `ctest --test-dir build -R tst_torrent --output-on-failure`
Expected: PASS — single-file and multi-file/selected downloads are byte-identical; resume finishes; force re-check works.

- [ ] **Step 5: Commit** — `git commit -am "feat(core): add TorrentTask (leech engine, resume, verify)"`.

---

### Task 11: DownloadManager.addTorrent + session persistence

**Files:**
- Modify: `src/core/DownloadManager.h`/`.cpp`
- Test: `tests/tst_torrent.cpp` (manager cases)

**Interfaces:**
- Consumes: `TorrentTask`, `TorrentMetainfo::parse`.
- Produces:
  ```cpp
  // DownloadManager:
  QUuid addTorrent(const QString& torrentPath, const QString& destDir,
                   const QSet<int>& selectedFiles, PieceStrategy strategy);
  // session JSON gains a "torrents" array (see spec §12)
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
void addTorrentCreatesRowAndCopiesFile() {
    QTemporaryDir data; DownloadManager mgr(EngineConfig{}, data.path());
    QString tpath = writeTorrentFixture(data.path());   // a valid .torrent on disk
    auto id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
    QVERIFY(!id.isNull());
    QCOMPARE(mgr.tasks().size(), 1);
    QCOMPARE(mgr.taskById(id)->kind(), AbstractTask::Kind::Torrent);
    QVERIFY(QFile::exists(data.path()+"/torrents/"+hexInfoHash+".torrent"));
}
void rejectsMalformedTorrent() {
    QTemporaryDir data; DownloadManager mgr(EngineConfig{}, data.path());
    QString bad = writeBytes(data.path()+"/bad.torrent", "not bencode");
    QVERIFY(mgr.addTorrent(bad, data.path(), {}, PieceStrategy::Sequential).isNull());
}
void sessionRoundTripsTorrent() {
    // addTorrent, saveSession, new manager, loadSession -> torrent task present with same selection/strategy
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement** — `addTorrent`: read file, `TorrentMetainfo::parse` (null id on failure), reject duplicate infoHash (return existing id), copy bytes to `<dataDir>/torrents/<hexInfoHash>.torrent`, construct `TorrentTask` (pass `<dataDir>/torrents` as resumeDir, the manager's `&m_limiter`, `m_logger`, a NAM), `wire()`, append, `pump()`. `wire()`: connect base `progress`/`stateChanged` to `taskProgress`/`taskStateChanged`. Extend `saveSession`/`loadSession` with a `torrents` array `{infoHash, destDir, selectedFiles, strategy, state}`; on load, re-read the stored `.torrent`, build the task, `restoreBitfield()`.

- [ ] **Step 4: Run** — `ctest -R tst_torrent` PASS (manager cases green).

- [ ] **Step 5: Commit** — `git commit -am "feat(core): DownloadManager.addTorrent + torrent session persistence"`.

---

### Task 12: BitTorrentPrefs in settings

**Files:**
- Modify: `src/gui/Settings.h`, `src/gui/Settings.cpp` (`SettingsIo::toJson`/`fromJson`)
- Test: `tests/tst_settings.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct BitTorrentPrefs {
      int maxPeersPerTorrent = 50;
      ResumeVerifyMode verify = ResumeVerifyMode::TrustBitfield;
      PieceStrategy defaultStrategy = PieceStrategy::RarestFirst;
      quint16 listenPort = 6881;
  };
  // AppSettings gains: BitTorrentPrefs bittorrent;
  ```

- [ ] **Step 1: Write the failing test**

```cpp
void bittorrentPrefsRoundTrip() {
    AppSettings s; s.bittorrent.maxPeersPerTorrent = 80;
    s.bittorrent.verify = ResumeVerifyMode::RecheckOnOpen;
    s.bittorrent.defaultStrategy = PieceStrategy::Sequential;
    auto j = SettingsIo::toJson(s);
    auto back = SettingsIo::fromJson(j);
    QCOMPARE(back.bittorrent.maxPeersPerTorrent, 80);
    QCOMPARE(int(back.bittorrent.verify), int(ResumeVerifyMode::RecheckOnOpen));
    QCOMPARE(int(back.bittorrent.defaultStrategy), int(PieceStrategy::Sequential));
}
void bittorrentPrefsDefaultsWhenAbsent() {
    auto back = SettingsIo::fromJson(QJsonObject{});   // no "bittorrent" key
    QCOMPARE(back.bittorrent.maxPeersPerTorrent, 50);
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement** — add `BitTorrentPrefs bittorrent;` to `AppSettings`; serialize a `"bittorrent"` object in `toJson`; parse it in `fromJson` with the defaults above when the key/fields are absent (tolerant, preserves unknown keys like the rest of `SettingsIo`).

- [ ] **Step 4: Run** — `ctest -R tst_settings` PASS.

- [ ] **Step 5: Commit** — `git commit -am "feat(gui): persist BitTorrent preferences"`.

---

### Task 13: TorrentOpenDialog + selection logic

**Files:**
- Create: `src/gui/TorrentOpenDialog.h`/`.cpp`, `src/gui/TorrentSelection.h`/`.cpp` (pure logic in `orbitgui_logic`)
- Modify: `src/gui/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/tst_gui.cpp` (selection-logic cases; dialog constructed offscreen)

**Interfaces:**
- Consumes: `TorrentMetainfo`.
- Produces:
  ```cpp
  // TorrentSelection.h (pure, orbitgui_logic)
  namespace TorrentSelection {
      struct Node { QString name; qint64 size; int fileIndex; QVector<int> children; }; // -1 = folder
      QVector<Node> buildTree(const TorrentMetainfo& m);       // paths -> tree
      QSet<int> selectedFileIndices(const QVector<Node>& tree, const QSet<int>& checkedNodes);
  }
  // TorrentOpenDialog: exec() -> destDir(), selectedFiles(), strategy()
  ```

- [ ] **Step 1: Write the failing tests** (pure selection logic)

```cpp
void buildsFileTreeFromMultiFileTorrent() {
    auto m = twoFileMeta();                       // "root/a.txt", "root/b.txt"
    auto tree = TorrentSelection::buildTree(m);
    QVERIFY(tree.size() >= 3);                     // root folder + 2 files
}
void selectedIndicesFollowChecks() {
    auto m = twoFileMeta(); auto tree = TorrentSelection::buildTree(m);
    auto sel = TorrentSelection::selectedFileIndices(tree, /*only file B node*/ {2});
    QCOMPARE(sel, QSet<int>{1});
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement** — `buildTree`: split each `FileEntry.path` on `/`, fold into a node tree (folders `fileIndex=-1`). `selectedFileIndices`: collect `fileIndex>=0` from checked leaf nodes. `TorrentOpenDialog`: a `QDialog` with a `QTreeWidget` (checkable, all checked by default), a destination `QLineEdit`+Browse (default download dir), a strategy `QComboBox` (default from `BitTorrentPrefs.defaultStrategy`), OK/Cancel; getters return the chosen values.

- [ ] **Step 4: Run** — `ctest -R tst_gui` PASS (selection cases).

- [ ] **Step 5: Commit** — `git commit -am "feat(gui): torrent open dialog with file selection"`.

---

### Task 14: MainWindow wiring — Open Torrent, drag&drop, grid, context menu, Preferences

**Files:**
- Modify: `src/gui/MainWindow.h`/`.cpp`, `src/gui/DropTargets.cpp`, `src/gui/ProgressGridWidget.h`/`.cpp`, `src/gui/ContextMenuRules.h`, `src/gui/PreferencesDialog.cpp`, `src/gui/DownloadTableModel.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadManager::addTorrent`, `TorrentTask` (via `qobject_cast`), `TorrentOpenDialog`, `BitTorrentPrefs`.

- [ ] **Step 1: Write the failing tests**

```cpp
void dropTargetsRecognizeTorrentExtension() {
    QVERIFY(DropTargets::isTorrentPath("/x/y.torrent"));
    QVERIFY(!DropTargets::isTorrentPath("http://h/f.zip"));
}
void gridMapsPiecesToCellsForTorrentRow() {
    // build a TorrentTask with N pieces, some Have; ask ProgressGridWidget for its cell model
    // assert cell count and that a Have piece maps to the Have color bucket.
}
void contextMenuOffersStrategyAndRecheckForTorrent() {
    // ContextMenuRules::buildFor(kind=Torrent) includes "Rarest-first"/"Sequential"/"Force re-check";
    // for an HTTP task it does not.
}
```

- [ ] **Step 2: Run to verify it fails** — FAIL to compile.

- [ ] **Step 3: Implement** —
  - `File > Open Torrent…`: `QFileDialog` (`*.torrent`) → `TorrentMetainfo::parse` (error box on failure) → `TorrentOpenDialog` → `mgr.addTorrent(...)`.
  - `DropTargets::isTorrentPath` + route a dropped `.torrent` (by extension / bencode sniff) through the same open flow.
  - `ProgressGridWidget`: when the selected row is a torrent (`qobject_cast<TorrentTask*>`), build cells from `pieceState(i)` (aggregate `ceil(pieceCount/tileBudget)` pieces per cell; cell color = worst/most-progressed bucket) and repaint on `pieceStateChanged`; byte downloads unchanged.
  - `ContextMenuRules`: add torrent-only items (checkable strategy submenu → `TorrentTask::setStrategy` + persist selection in the session; **Force re-check** → `TorrentTask::forceRecheck`).
  - `PreferencesDialog`: new **BitTorrent** sidebar category with Max peers, Resume verification, Default strategy, Listen port bound to `BitTorrentPrefs`; apply live where possible on OK.
  - `DownloadTableModel`: ensure `Checking` renders; torrent rows read name/size/state via `AbstractTask`.

- [ ] **Step 4: Run the full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all targets PASS, including the unchanged `tst_download` gate; `orbit-gui` builds.

- [ ] **Step 5: Commit** — `git commit -am "feat(gui): open torrents, piece grid, context menu, BitTorrent preferences"`.

---

### Task 15: Final integration pass — headless boot + docs

**Files:**
- Modify: `README.md` (BitTorrent MVP note), `docs/superpowers/specs/2026-07-23-bittorrent-mvp-leech-design.md` (Status → Implemented), any follow-up notes
- Test: full `ctest` + `orbit-gui` smoke

- [ ] **Step 1: Run the full suite and headless GUI smoke**

Run: `ctest --test-dir build --output-on-failure` — Expected: all PASS.
Run: `QT_QPA_PLATFORM=offscreen ./build/src/gui/orbit-gui` (starts, no crash), then quit.

- [ ] **Step 2: Verify no `tst_download` expectation drift** — diff the case count/expectations against `main`/`develop`; confirm the gate held.

- [ ] **Step 3: Update docs** — README: a short "BitTorrent (leech-only)" line under features with the MVP scope; spec Status line to "Implemented (pending human E2E)". List the accepted MVP limitations (no endgame, HTTP tracker only, no seed) for the human E2E.

- [ ] **Step 4: Commit** — `git commit -am "docs: BitTorrent MVP leech-only shipped; note scope and limits"`.

- [ ] **Step 5: Hand off for human E2E** — real `.torrent` with a live HTTP tracker: open (menu + drag&drop), file selection, download to completion with SHA-1 verify, pause/resume across restart, strategy switch, Force re-check, bandwidth cap. These are not automatable and are the maintainer's gate before merge.

---

## Self-Review

**Spec coverage:** Bencode (T1), metainfo/info-hash (T2), bitfield (T3), piece store/mapping/selection/verify (T4), piece picker both strategies (T5), AbstractTask seam + `Checking` (T6), HTTP tracker (T7), peer wire/PeerConnection download-only (T8), TestSeeder (T9), TorrentTask engine/resume/RateLimiter (T10), manager `addTorrent`/session (T11), settings (T12), open dialog/selection (T13), MainWindow/grid/context-menu/Preferences (T14), integration/docs (T15). All spec §Design and §Data/Settings items map to a task; the two accepted MVP limitations (no endgame, HTTP-tracker only) are called out for the human E2E.

**Placeholder scan:** Test bodies with `// ...` are fixture-construction helpers (metainfo/data builders) whose signatures are named; every step shows real interfaces and the key logic. No "TBD/handle edge cases/similar to Task N".

**Type consistency:** `PieceStrategy`/`ResumeVerifyMode`/`PieceState` are defined in `DownloadTypes.h` (T5/T6/T10) and reused verbatim in T12–T14; `PeerAddress` defined in T7 and reused in T8/T10; `addTorrent`/`AbstractTask::Kind` names match across T6/T11/T14.

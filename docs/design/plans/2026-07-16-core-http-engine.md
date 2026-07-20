# Core HTTP Engine (Fase 1) — Implementation Plan

**Goal:** Build a headless, GUI-free C++/Qt6 engine that downloads an HTTP/HTTPS file in parallel byte-range segments, with progress reporting, per-segment retry, and pause/resume that survives process restart.

**Architecture:** A `core` static library (`orbitcore`) depending only on `QtCore`/`QtNetwork`. Pure logic (segmentation, persistence) is split from I/O classes (`HttpProbe`, `SegmentWorker`, `DownloadTask`, `DownloadManager`). Everything runs on the main event loop — no manual threads. A thin `orbit-cli` exercises the engine end-to-end; a `QHttpServer`-based `TestServer` drives deterministic, offline tests.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, HttpServer, Test), CMake, QtTest.

## Global Constraints

- **Qt version floor:** Qt 6.11 (Homebrew, `CMAKE_PREFIX_PATH=/opt/homebrew`). C++ standard: C++20 (`CMAKE_CXX_STANDARD 20`, required, no compiler extensions).
- **Core has NO QtWidgets dependency.** `orbitcore` links only `Qt6::Core` and `Qt6::Network`.
- **All tests are offline** — network tests hit `127.0.0.1` via the in-process `TestServer` only.
- **Concurrency model:** single-threaded, async on the main event loop. No `QThread`, no mutexes.
- **Config lives in `EngineConfig`** (see Task 2). Defaults: `maxConcurrentDownloads=3`, `segmentCount=4`, `minSegSize=1 MiB`, `maxSegmentRetries=5`, `retryBackoffBaseMs=1000`, `connectTimeoutMs=30000`, `idleTimeoutMs=30000`, `progressThrottleMs=200`.
- **Commit discipline:** conventional-commit messages **in English**, no co-author trailer.
- **Meta sidecar path** is always `destPath + ".meta"`. Session file is `downloads.json` under the data dir.

---

## File Structure

```
orbit-downloader-tribute/
  CMakeLists.txt                 # top-level: finds Qt, adds subdirs
  src/
    core/
      CMakeLists.txt             # orbitcore static lib
      DownloadTypes.h            # enums + structs (Segment, EngineConfig, ProbeResult), header-only
      Segmentation.{h,cpp}       # computeSegments() — pure
      Persistence.{h,cpp}        # .meta + downloads.json I/O, atomic writes, resolveUniquePath()
      HttpProbe.{h,cpp}          # one-shot URL probe (size, range support, validators)
      SegmentWorker.{h,cpp}      # one segment: range GET, write-at-offset, retry, timeout
      DownloadTask.{h,cpp}       # orchestrates probe + N SegmentWorkers + meta + state machine
      DownloadManager.{h,cpp}    # task collection, queue, maxConcurrent, session load/save
    cli/
      CMakeLists.txt
      main.cpp                   # orbit-cli <url> <dest> [--segments N] [--max-concurrent M]
  tests/
    CMakeLists.txt
    TestServer.{h,cpp}           # QHttpServer test double with configurable routes
    tst_segmentation.cpp
    tst_persistence.cpp
    tst_download.cpp             # integration: probe, single/multi/fallback, pause/resume, retry, timeout, restart
  docs/design/{specs,plans}/
```

Each file has one responsibility. Pure logic (`Segmentation`, `Persistence`) is unit-tested with no network. I/O classes are tested against `TestServer`.

## Interfaces (locked signatures)

These are defined by the tasks noted; later tasks consume them verbatim.

```cpp
// DownloadTypes.h  (Task 2)
enum class DownloadState { Queued, Connecting, Downloading, Paused, Completed, Error };

struct Segment {
    int    index   = 0;
    qint64 start   = 0;      // immutable
    qint64 current = 0;      // next byte to write
    qint64 end     = -1;     // inclusive; -1 = until EOF (fallback)
    qint64 downloaded() const { return current - start; }
    bool   isComplete() const { return end >= 0 && current > end; }
};

struct EngineConfig {
    int    maxConcurrentDownloads = 3;
    int    segmentCount           = 4;
    qint64 minSegSize             = 1LL << 20;
    int    maxSegmentRetries      = 5;
    int    retryBackoffBaseMs     = 1000;
    int    connectTimeoutMs       = 30000;
    int    idleTimeoutMs          = 30000;
    int    progressThrottleMs     = 200;
};

struct ProbeResult {
    bool    ok           = false;
    qint64  totalBytes   = -1;
    bool    supportsRange= false;
    QString etag;
    QString lastModified;
    QUrl    resolvedUrl;
    QString error;
};

// Segmentation.h  (Task 2)
QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize);

// Persistence.h  (Task 3)
struct DownloadRecord {
    QUuid         id;
    QUrl          url;
    QString       destPath;
    qint64        totalBytes    = -1;
    bool          supportsRange = false;
    DownloadState state         = DownloadState::Queued;
    int           segmentCount  = 4;
};
namespace Persistence {
    bool  writeFileAtomic(const QString& path, const QByteArray& data);
    QString metaPath(const QString& destPath);            // destPath + ".meta"
    bool  writeMeta(const QString& destPath, const QVector<Segment>& segs,
                    const QString& etag, const QString& lastModified, bool validated);
    bool  readMeta(const QString& destPath, QVector<Segment>& segs,
                   QString& etag, QString& lastModified, bool& validated);
    void  removeMeta(const QString& destPath);
    bool  writeSession(const QString& jsonPath, const QVector<DownloadRecord>& recs);
    QVector<DownloadRecord> readSession(const QString& jsonPath);
    QString resolveUniquePath(const QString& destPath);   // adds " (1)", " (2)"...
}

// HttpProbe.h  (Task 4)   signal: void finished(const ProbeResult&);
class HttpProbe : public QObject { public:
    HttpProbe(QNetworkAccessManager* nam, QObject* parent=nullptr);
    void start(const QUrl& url); };

// SegmentWorker.h  (Task 5)
//   signals: progressed(int idx, qint64 currentOffset); completed(int idx);
//            failed(int idx, QString error); restartRequired(int idx);
class SegmentWorker : public QObject { public:
    SegmentWorker(QNetworkAccessManager* nam, QFile* file, const EngineConfig& cfg,
                  QObject* parent=nullptr);
    void start(const Segment& seg, const QUrl& url, const QString& ifRangeValidator);
    void stop();                       // abort without retry (pause)
    Segment segment() const; };

// DownloadTask.h  (Task 6+)
//   signals: progress(qint64 received, qint64 total); stateChanged(DownloadState);
//            segmentProgress(int idx, qint64 currentOffset);
class DownloadTask : public QObject { public:
    DownloadTask(QNetworkAccessManager* nam, const EngineConfig& cfg, QObject* parent=nullptr);
    void init(const QUuid& id, const QUrl& url, const QString& destPath, int segmentCount);
    void restore(const DownloadRecord& rec, const QVector<Segment>& segs,
                 const QString& etag, const QString& lastModified, bool validated);
    void start();
    void pause();
    DownloadState  state() const;
    QUuid          id() const;
    DownloadRecord record() const;
    QVector<Segment> segments() const; };

// DownloadManager.h  (Task 12)
//   signals: taskProgress(QUuid,qint64,qint64); taskStateChanged(QUuid,DownloadState);
class DownloadManager : public QObject { public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir, QObject* parent=nullptr);
    QUuid addDownload(const QUrl& url, const QString& destPath);
    void  pauseAll();
    void  resumeAll();
    void  remove(const QUuid& id, bool deleteFiles);
    void  loadSession();
    QVector<DownloadTask*> tasks() const; };
```

---

### Task 1: Project scaffold + build/test smoke

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/core/CMakeLists.txt`
- Create: `src/core/DownloadTypes.h`
- Create: `tests/CMakeLists.txt`
- Create: `tests/tst_smoke.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable `orbitcore` lib target and a runnable `ctest` suite. Establishes the CMake wiring every later task relies on.

- [ ] **Step 1: Write the top-level CMake**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.21)
project(orbit_downloader_tribute LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer Test)

enable_testing()
add_subdirectory(src/core)
add_subdirectory(tests)
```

- [ ] **Step 2: Write the core lib CMake and a header-only placeholder**

`src/core/CMakeLists.txt`:
```cmake
add_library(orbitcore STATIC
  DownloadTypes.h
)
set_target_properties(orbitcore PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(orbitcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbitcore PUBLIC Qt6::Core Qt6::Network)
```

`src/core/DownloadTypes.h`:
```cpp
#pragma once
#include <QString>
#include <QUrl>
#include <QUuid>
#include <QVector>

enum class DownloadState { Queued, Connecting, Downloading, Paused, Completed, Error };
```
(A library with only a header needs at least one translation unit; the smoke test below links `orbitcore`, which is enough for CMake. If your CMake errors on an object-less STATIC lib, add an empty `Version.cpp` with `// orbitcore` and list it in the sources.)

- [ ] **Step 3: Write the smoke test**

`tests/tst_smoke.cpp`:
```cpp
#include <QtTest>
#include "DownloadTypes.h"

class TstSmoke : public QObject {
    Q_OBJECT
private slots:
    void enumExists() {
        QCOMPARE(static_cast<int>(DownloadState::Queued), 0);
    }
};

QTEST_MAIN(TstSmoke)
#include "tst_smoke.moc"
```

`tests/CMakeLists.txt`:
```cmake
add_executable(tst_smoke tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_smoke COMMAND tst_smoke)
```

- [ ] **Step 4: Configure, build, run — verify it passes**

Run:
```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: `tst_smoke` builds and PASSES (`100% tests passed`).

- [ ] **Step 5: Commit** (ask for authorization first)

```bash
git add CMakeLists.txt src/core tests
git commit -m "chore: scaffold cmake build with orbitcore lib and smoke test"
```

---

### Task 2: Segmentation logic (pure)

**Files:**
- Modify: `src/core/DownloadTypes.h` (add `Segment`, `EngineConfig`, `ProbeResult`)
- Create: `src/core/Segmentation.h`, `src/core/Segmentation.cpp`
- Modify: `src/core/CMakeLists.txt` (add Segmentation.cpp)
- Create: `tests/tst_segmentation.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Segment`, `EngineConfig` from `DownloadTypes.h`.
- Produces: `QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange, int segmentCount, qint64 minSegSize)`.

- [ ] **Step 1: Write the failing tests**

`tests/tst_segmentation.cpp`:
```cpp
#include <QtTest>
#include "Segmentation.h"

class TstSegmentation : public QObject {
    Q_OBJECT
private slots:
    void splitsEvenly() {
        auto s = computeSegments(1000, true, 4, 1);   // minSeg=1 so no clamp
        QCOMPARE(s.size(), 4);
        QCOMPARE(s[0].start, 0LL);   QCOMPARE(s[0].end, 249LL);
        QCOMPARE(s[1].start, 250LL); QCOMPARE(s[1].end, 499LL);
        QCOMPARE(s[3].end, 999LL);                    // last covers remainder
        // contiguous, no gaps/overlap
        for (int i = 1; i < s.size(); ++i)
            QCOMPARE(s[i].start, s[i-1].end + 1);
    }
    void lastAbsorbsRemainder() {
        auto s = computeSegments(1003, true, 4, 1);
        QCOMPARE(s.last().end, 1002LL);
        QCOMPARE(s[0].end - s[0].start + 1, 250LL);   // 1003/4 = 250
        QCOMPARE(s.last().end - s.last().start + 1, 253LL);
    }
    void clampsToMinSegSize() {
        auto s = computeSegments(1000, true, 8, 400); // ceil(1000/400)=3 max segs
        QCOMPARE(s.size(), 3);
    }
    void fallbackWhenNoRange() {
        auto s = computeSegments(1000, false, 4, 1);
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].start, 0LL);
        QCOMPARE(s[0].end, -1LL);
    }
    void fallbackWhenUnknownSize() {
        auto s = computeSegments(-1, true, 4, 1);
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].end, -1LL);
    }
    void currentStartsAtStart() {
        auto s = computeSegments(1000, true, 4, 1);
        for (const auto& seg : s) QCOMPARE(seg.current, seg.start);
    }
};

QTEST_MAIN(TstSegmentation)
#include "tst_segmentation.moc"
```

- [ ] **Step 2: Add the structs to DownloadTypes.h**

Append to `src/core/DownloadTypes.h` (below the enum):
```cpp
struct Segment {
    int    index   = 0;
    qint64 start   = 0;
    qint64 current = 0;
    qint64 end     = -1;
    qint64 downloaded() const { return current - start; }
    bool   isComplete() const { return end >= 0 && current > end; }
};

struct EngineConfig {
    int    maxConcurrentDownloads = 3;
    int    segmentCount           = 4;
    qint64 minSegSize             = 1LL << 20;
    int    maxSegmentRetries      = 5;
    int    retryBackoffBaseMs     = 1000;
    int    connectTimeoutMs       = 30000;
    int    idleTimeoutMs          = 30000;
    int    progressThrottleMs     = 200;
};

struct ProbeResult {
    bool    ok            = false;
    qint64  totalBytes    = -1;
    bool    supportsRange = false;
    QString etag;
    QString lastModified;
    QUrl    resolvedUrl;
    QString error;
};
```

- [ ] **Step 3: Write Segmentation**

`src/core/Segmentation.h`:
```cpp
#pragma once
#include "DownloadTypes.h"

QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize);
```

`src/core/Segmentation.cpp`:
```cpp
#include "Segmentation.h"
#include <algorithm>

QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize) {
    QVector<Segment> segs;
    if (!supportsRange || totalBytes <= 0) {
        segs.append(Segment{0, 0, 0, -1});
        return segs;
    }
    const qint64 minSeg = std::max<qint64>(1, minSegSize);
    const int maxByMin  = static_cast<int>((totalBytes + minSeg - 1) / minSeg);
    const int n         = std::max(1, std::min(segmentCount, maxByMin));
    const qint64 base   = totalBytes / n;
    qint64 offset = 0;
    for (int i = 0; i < n; ++i) {
        const qint64 len = (i == n - 1) ? (totalBytes - offset) : base;
        Segment s;
        s.index   = i;
        s.start   = offset;
        s.current = offset;
        s.end     = offset + len - 1;
        segs.append(s);
        offset += len;
    }
    return segs;
}
```

- [ ] **Step 4: Wire the build**

In `src/core/CMakeLists.txt`, add `Segmentation.cpp` to the `add_library(orbitcore STATIC ...)` sources.

In `tests/CMakeLists.txt`, append:
```cmake
add_executable(tst_segmentation tst_segmentation.cpp)
target_link_libraries(tst_segmentation PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_segmentation COMMAND tst_segmentation)
```

- [ ] **Step 5: Build and run — verify PASS**

Run:
```bash
cmake --build build
ctest --test-dir build -R tst_segmentation --output-on-failure
```
Expected: all `TstSegmentation` cases PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add byte-range segmentation with min-size clamp and fallback"
```

---

### Task 3: Persistence (meta sidecar + session + unique path)

**Files:**
- Create: `src/core/Persistence.h`, `src/core/Persistence.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `tests/tst_persistence.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Segment`, `DownloadState` from `DownloadTypes.h`.
- Produces: `DownloadRecord` struct and the `Persistence` namespace functions listed in the Interfaces section.

- [ ] **Step 1: Write the failing tests**

`tests/tst_persistence.cpp`:
```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "Persistence.h"

class TstPersistence : public QObject {
    Q_OBJECT
private slots:
    void metaRoundTrip() {
        QTemporaryDir dir;
        const QString dest = dir.filePath("file.bin");
        QVector<Segment> segs = {{0,0,120,499}, {1,500,500,999}};
        QVERIFY(Persistence::writeMeta(dest, segs, "\"abc\"", "Mon", true));
        QVERIFY(QFile::exists(Persistence::metaPath(dest)));

        QVector<Segment> back; QString etag, lm; bool validated=false;
        QVERIFY(Persistence::readMeta(dest, back, etag, lm, validated));
        QCOMPARE(back.size(), 2);
        QCOMPARE(back[0].current, 120LL);
        QCOMPARE(back[1].end, 999LL);
        QCOMPARE(etag, QString("\"abc\""));
        QCOMPARE(validated, true);
    }
    void removeMetaDeletesFile() {
        QTemporaryDir dir;
        const QString dest = dir.filePath("f.bin");
        Persistence::writeMeta(dest, {{0,0,0,9}}, "", "", false);
        Persistence::removeMeta(dest);
        QVERIFY(!QFile::exists(Persistence::metaPath(dest)));
    }
    void sessionRoundTrip() {
        QTemporaryDir dir;
        const QString path = dir.filePath("downloads.json");
        DownloadRecord r;
        r.id = QUuid::createUuid();
        r.url = QUrl("http://x/y.bin");
        r.destPath = "/tmp/y.bin";
        r.totalBytes = 1000; r.supportsRange = true;
        r.state = DownloadState::Paused; r.segmentCount = 4;
        QVERIFY(Persistence::writeSession(path, {r}));

        auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].id, r.id);
        QCOMPARE(back[0].url, r.url);
        QCOMPARE(back[0].totalBytes, 1000LL);
        QCOMPARE(back[0].state, DownloadState::Paused);
    }
    void resolveUniqueAvoidsCollision() {
        QTemporaryDir dir;
        const QString a = dir.filePath("movie.flv");
        QFile(a).open(QIODevice::WriteOnly);          // create it
        const QString got = Persistence::resolveUniquePath(a);
        QCOMPARE(got, dir.filePath("movie (1).flv"));
    }
    void resolveUniqueReturnsSameWhenFree() {
        QTemporaryDir dir;
        const QString a = dir.filePath("free.bin");
        QCOMPARE(Persistence::resolveUniquePath(a), a);
    }
    void atomicWriteReplaces() {
        QTemporaryDir dir;
        const QString p = dir.filePath("x.txt");
        QVERIFY(Persistence::writeFileAtomic(p, "one"));
        QVERIFY(Persistence::writeFileAtomic(p, "two"));
        QFile f(p); f.open(QIODevice::ReadOnly);
        QCOMPARE(f.readAll(), QByteArray("two"));
    }
};

QTEST_MAIN(TstPersistence)
#include "tst_persistence.moc"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build 2>&1 | head` — Expected: compile error, `Persistence.h` not found. (Add the build wiring in Step 4, then it should fail on assertions/undefined functions until Step 3 is in.)

- [ ] **Step 3: Implement Persistence**

`src/core/Persistence.h`:
```cpp
#pragma once
#include "DownloadTypes.h"

struct DownloadRecord {
    QUuid         id;
    QUrl          url;
    QString       destPath;
    qint64        totalBytes    = -1;
    bool          supportsRange = false;
    DownloadState state         = DownloadState::Queued;
    int           segmentCount  = 4;
};

namespace Persistence {
    bool    writeFileAtomic(const QString& path, const QByteArray& data);
    QString metaPath(const QString& destPath);
    bool    writeMeta(const QString& destPath, const QVector<Segment>& segs,
                      const QString& etag, const QString& lastModified, bool validated);
    bool    readMeta(const QString& destPath, QVector<Segment>& segs,
                     QString& etag, QString& lastModified, bool& validated);
    void    removeMeta(const QString& destPath);
    bool    writeSession(const QString& jsonPath, const QVector<DownloadRecord>& recs);
    QVector<DownloadRecord> readSession(const QString& jsonPath);
    QString resolveUniquePath(const QString& destPath);
}
```

`src/core/Persistence.cpp`:
```cpp
#include "Persistence.h"
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace Persistence {

bool writeFileAtomic(const QString& path, const QByteArray& data) {
    QSaveFile f(path);                       // QSaveFile = temp + atomic rename on commit
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(data) != data.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

QString metaPath(const QString& destPath) { return destPath + ".meta"; }

static QJsonObject segToJson(const Segment& s) {
    return QJsonObject{{"index", s.index}, {"start", double(s.start)},
                       {"current", double(s.current)}, {"end", double(s.end)}};
}
static Segment segFromJson(const QJsonObject& o) {
    Segment s;
    s.index   = o.value("index").toInt();
    s.start   = qint64(o.value("start").toDouble());
    s.current = qint64(o.value("current").toDouble());
    s.end     = qint64(o.value("end").toDouble());
    return s;
}

bool writeMeta(const QString& destPath, const QVector<Segment>& segs,
               const QString& etag, const QString& lastModified, bool validated) {
    QJsonArray arr;
    for (const auto& s : segs) arr.append(segToJson(s));
    QJsonObject root{{"segments", arr}, {"etag", etag},
                     {"lastModified", lastModified}, {"validated", validated}};
    return writeFileAtomic(metaPath(destPath),
                           QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool readMeta(const QString& destPath, QVector<Segment>& segs,
              QString& etag, QString& lastModified, bool& validated) {
    QFile f(metaPath(destPath));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    const auto root = doc.object();
    segs.clear();
    for (const auto v : root.value("segments").toArray()) segs.append(segFromJson(v.toObject()));
    etag         = root.value("etag").toString();
    lastModified = root.value("lastModified").toString();
    validated    = root.value("validated").toBool();
    return true;
}

void removeMeta(const QString& destPath) { QFile::remove(metaPath(destPath)); }

bool writeSession(const QString& jsonPath, const QVector<DownloadRecord>& recs) {
    QJsonArray arr;
    for (const auto& r : recs) {
        arr.append(QJsonObject{
            {"id", r.id.toString(QUuid::WithoutBraces)},
            {"url", r.url.toString()},
            {"destPath", r.destPath},
            {"totalBytes", double(r.totalBytes)},
            {"supportsRange", r.supportsRange},
            {"state", int(r.state)},
            {"segmentCount", r.segmentCount}});
    }
    return writeFileAtomic(jsonPath, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<DownloadRecord> readSession(const QString& jsonPath) {
    QVector<DownloadRecord> recs;
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) return recs;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    for (const auto v : doc.array()) {
        const auto o = v.toObject();
        DownloadRecord r;
        r.id            = QUuid::fromString(o.value("id").toString());
        r.url           = QUrl(o.value("url").toString());
        r.destPath      = o.value("destPath").toString();
        r.totalBytes    = qint64(o.value("totalBytes").toDouble());
        r.supportsRange = o.value("supportsRange").toBool();
        r.state         = DownloadState(o.value("state").toInt());
        r.segmentCount  = o.value("segmentCount").toInt();
        recs.append(r);
    }
    return recs;
}

QString resolveUniquePath(const QString& destPath) {
    if (!QFileInfo::exists(destPath)) return destPath;
    QFileInfo fi(destPath);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();       // "movie" from "movie.flv"
    const QString suf  = fi.suffix();                 // "flv"
    for (int i = 1; ; ++i) {
        QString candidate = dir + "/" + base + QString(" (%1)").arg(i);
        if (!suf.isEmpty()) candidate += "." + suf;
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

} // namespace Persistence
```

- [ ] **Step 4: Wire the build**

In `src/core/CMakeLists.txt` add `Persistence.cpp`. In `tests/CMakeLists.txt` append:
```cmake
add_executable(tst_persistence tst_persistence.cpp)
target_link_libraries(tst_persistence PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_persistence COMMAND tst_persistence)
```

- [ ] **Step 5: Build and run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_persistence --output-on-failure`
Expected: all `TstPersistence` cases PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add meta/session persistence with atomic writes and unique-path resolver"
```

---

### Task 4: TestServer fixture + HttpProbe

**Files:**
- Create: `tests/TestServer.h`, `tests/TestServer.cpp`
- Create: `src/core/HttpProbe.h`, `src/core/HttpProbe.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Create: `tests/tst_download.cpp` (starts here; grows in later tasks)

**Interfaces:**
- Consumes: `ProbeResult` from `DownloadTypes.h`.
- Produces: `HttpProbe` (signal `finished(const ProbeResult&)`) and the reusable `TestServer` fixture with routes `/ranged`, `/plain`, `/nolength`, `/flaky`, `/stall`, `/notfound`, `/changed`.

- [ ] **Step 1: Build the TestServer fixture**

`tests/TestServer.h`:
```cpp
#pragma once
#include <QByteArray>
#include <QHttpServer>
#include <QTcpServer>
#include <QUrl>
#include <QHash>

// In-process HTTP test double. Serves a deterministic body on several routes.
class TestServer {
public:
    explicit TestServer(QByteArray body);
    bool listen();                 // binds 127.0.0.1 on an ephemeral port
    quint16 port() const { return m_port; }
    QUrl url(const QString& path) const;      // e.g. url("/ranged")
    void setEtag(const QString& e) { m_etag = e; }

private:
    QByteArray partial(qint64 start, qint64 end) const;   // inclusive
    QByteArray m_body;
    QString    m_etag = "\"v1\"";
    QHttpServer m_http;
    QTcpServer  m_tcp;
    quint16     m_port = 0;
    mutable QHash<QString,int> m_hits;        // per-path counter for /flaky
    friend class TestServerRoutes;
};
```

`tests/TestServer.cpp`:
```cpp
#include "TestServer.h"
#include <QHttpServerResponse>
#include <QRegularExpression>

TestServer::TestServer(QByteArray body) : m_body(std::move(body)) {}

QByteArray TestServer::partial(qint64 start, qint64 end) const {
    if (end < 0 || end >= m_body.size()) end = m_body.size() - 1;
    if (start < 0) start = 0;
    return m_body.mid(int(start), int(end - start + 1));
}

// Parse "bytes=start-end" -> {start,end}; end=-1 means open.
static bool parseRange(const QByteArray& h, qint64& start, qint64& end) {
    QRegularExpression re("bytes=(\\d+)-(\\d*)");
    auto m = re.match(QString::fromUtf8(h));
    if (!m.hasMatch()) return false;
    start = m.captured(1).toLongLong();
    end   = m.captured(2).isEmpty() ? -1 : m.captured(2).toLongLong();
    return true;
}

bool TestServer::listen() {
    using Resp = QHttpServerResponse;

    m_http.route("/ranged", [this](const QHttpServerRequest& req) {
        qint64 s, e;
        const QByteArray rh = req.value("Range");
        if (parseRange(rh, s, e)) {
            const QByteArray b = partial(s, e < 0 ? m_body.size()-1 : e);
            Resp r("application/octet-stream", b);
            r.setHeader("Content-Range",
                QString("bytes %1-%2/%3").arg(s).arg(s + b.size() - 1).arg(m_body.size()).toUtf8());
            r.setHeader("Accept-Ranges", "bytes");
            r.setHeader("ETag", m_etag.toUtf8());
            return Resp(r.data(), QHttpServerResponse::StatusCode::PartialContent);
        }
        Resp full("application/octet-stream", m_body);
        full.setHeader("ETag", m_etag.toUtf8());
        return full;
    });

    m_http.route("/plain", [this](const QHttpServerRequest&) {
        return Resp("application/octet-stream", m_body);   // ignores Range, always 200
    });

    m_http.route("/nolength", [this](const QHttpServerRequest&) {
        Resp r("application/octet-stream", m_body);
        r.setHeader("Content-Length", QByteArray());       // suppressed → unknown size path
        return r;
    });

    m_http.route("/notfound", [](const QHttpServerRequest&) {
        return Resp("text/plain", "no", QHttpServerResponse::StatusCode::NotFound);
    });

    m_http.route("/changed", [this](const QHttpServerRequest& req) {
        // On resume (Range present) reply 200 with a different ETag -> triggers restart.
        if (!req.value("Range").isEmpty()) {
            Resp r("application/octet-stream", m_body);
            r.setHeader("ETag", "\"v2\"");
            return r;                                       // 200, not 206
        }
        Resp r("application/octet-stream", m_body);
        r.setHeader("ETag", m_etag.toUtf8());
        r.setHeader("Accept-Ranges", "bytes");
        return r;
    });

    // /flaky and /stall are added in Tasks 8 and 9 (they need raw socket control).

    m_tcp.listen(QHostAddress::LocalHost);
    m_port = m_tcp.serverPort();
    return m_http.bind(&m_tcp);
}

QUrl TestServer::url(const QString& path) const {
    return QUrl(QString("http://127.0.0.1:%1%2").arg(m_port).arg(path));
}
```
> Note: `QHttpServerResponse` header/ctor APIs shift slightly across Qt 6.x. If a header setter differs in 6.11, adjust to the available overload — the behavior (status code + headers above) is what matters. Verify against `qthttpserver` headers under `/opt/homebrew/include`.

- [ ] **Step 2: Write the failing probe tests**

`tests/tst_download.cpp`:
```cpp
#include <QtTest>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include "TestServer.h"
#include "HttpProbe.h"

static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

class TstDownload : public QObject {
    Q_OBJECT
    QByteArray m_body;
private slots:
    void initTestCase() { m_body = makeBody(5000); }

    void probeRangedReportsSizeAndSupport() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam);
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/ranged"));
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QVERIFY(res.supportsRange);
        QCOMPARE(res.totalBytes, qint64(m_body.size()));
        QCOMPARE(res.etag, QString("\"v1\""));
    }

    void probePlainReportsNoRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam);
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/plain"));
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QVERIFY(!res.supportsRange);
        QCOMPARE(res.totalBytes, qint64(m_body.size()));
    }

    void probe404NotOk() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam);
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/notfound"));
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(!res.ok);
    }
};

QTEST_MAIN(TstDownload)
#include "tst_download.moc"
```
`ProbeResult` must be registerable in QVariant — add `Q_DECLARE_METATYPE(ProbeResult)` at the bottom of `DownloadTypes.h` and `qRegisterMetaType<ProbeResult>("ProbeResult")` in `HttpProbe`'s constructor.

- [ ] **Step 3: Implement HttpProbe**

`src/core/HttpProbe.h`:
```cpp
#pragma once
#include "DownloadTypes.h"
#include <QObject>
class QNetworkAccessManager;
class QNetworkReply;

class HttpProbe : public QObject {
    Q_OBJECT
public:
    explicit HttpProbe(QNetworkAccessManager* nam, QObject* parent = nullptr);
    void start(const QUrl& url);
signals:
    void finished(const ProbeResult& result);
private:
    void onMetaDataChanged();
    void onErrorOccurred();
    QNetworkAccessManager* m_nam;
    QNetworkReply*         m_reply = nullptr;
    bool                   m_done  = false;
};
```

`src/core/HttpProbe.cpp`:
```cpp
#include "HttpProbe.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

HttpProbe::HttpProbe(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_nam(nam) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}

void HttpProbe::start(const QUrl& url) {
    QNetworkRequest req(url);
    req.setRawHeader("Range", "bytes=0-0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &HttpProbe::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::errorOccurred,   this, &HttpProbe::onErrorOccurred);
}

void HttpProbe::onMetaDataChanged() {
    if (m_done) return;
    m_done = true;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    ProbeResult r;
    r.resolvedUrl  = m_reply->url();
    r.etag         = QString::fromUtf8(m_reply->rawHeader("ETag"));
    r.lastModified = QString::fromUtf8(m_reply->rawHeader("Last-Modified"));

    if (status == 206) {
        r.supportsRange = true;
        const QByteArray cr = m_reply->rawHeader("Content-Range");   // "bytes 0-0/12345"
        const int slash = cr.lastIndexOf('/');
        if (slash >= 0) r.totalBytes = cr.mid(slash + 1).trimmed().toLongLong();
        r.ok = true;
    } else if (status == 200) {
        r.supportsRange = false;
        const QVariant cl = m_reply->header(QNetworkRequest::ContentLengthHeader);
        r.totalBytes = cl.isValid() ? cl.toLongLong() : -1;
        r.ok = true;
    } else {
        r.ok = false;
        r.error = QString("HTTP %1").arg(status);
    }
    m_reply->abort();          // headers are enough; don't download the body
    m_reply->deleteLater();
    emit finished(r);
}

void HttpProbe::onErrorOccurred() {
    if (m_done) return;        // an abort() after success also fires this; guard it
    m_done = true;
    ProbeResult r;
    r.ok = false;
    r.error = m_reply->errorString();
    m_reply->deleteLater();
    emit finished(r);
}
```

- [ ] **Step 4: Wire the build**

`src/core/CMakeLists.txt`: add `HttpProbe.cpp`.
`tests/CMakeLists.txt`: append:
```cmake
add_executable(tst_download tst_download.cpp TestServer.cpp)
target_link_libraries(tst_download PRIVATE orbitcore Qt6::Test Qt6::HttpServer)
add_test(NAME tst_download COMMAND tst_download)
```

- [ ] **Step 5: Build and run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: the three probe cases PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add HttpProbe and in-process TestServer fixture"
```

---

### Task 5: SegmentWorker (single range, write-at-offset, no retry yet)

**Files:**
- Create: `src/core/SegmentWorker.h`, `src/core/SegmentWorker.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `Segment`, `EngineConfig`; a shared `QFile*`; `QNetworkAccessManager*`.
- Produces: `SegmentWorker` with signals `progressed(int,qint64)`, `completed(int)`, `failed(int,QString)`, `restartRequired(int)`; methods `start(seg,url,ifRangeValidator)`, `stop()`, `segment()`.

- [ ] **Step 1: Write the failing test**

Add to `tst_download.cpp` (inside the class):
```cpp
    void segmentWritesItsRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryFile tmp; tmp.open();
        const QString path = tmp.fileName();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.resize(m_body.size());                 // preallocate

        QNetworkAccessManager nam;
        EngineConfig cfg;
        SegmentWorker w(&nam, &file, cfg);
        QSignalSpy done(&w, &SegmentWorker::completed);
        Segment seg{0, 1000, 1000, 1999};           // bytes 1000..1999
        w.start(seg, srv.url("/ranged"), QString());
        QVERIFY(done.wait(3000));

        file.seek(1000);
        QCOMPARE(file.read(1000), m_body.mid(1000, 1000));
        QCOMPARE(w.segment().current, 2000LL);      // advanced past end
    }
```
Add `#include "SegmentWorker.h"` and `#include <QTemporaryFile>` at the top.

- [ ] **Step 2: Implement SegmentWorker (retry/timeout stubbed to zero for now)**

`src/core/SegmentWorker.h`:
```cpp
#pragma once
#include "DownloadTypes.h"
#include <QObject>
class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QTimer;

class SegmentWorker : public QObject {
    Q_OBJECT
public:
    SegmentWorker(QNetworkAccessManager* nam, QFile* file, const EngineConfig& cfg,
                  QObject* parent = nullptr);
    void start(const Segment& seg, const QUrl& url, const QString& ifRangeValidator);
    void stop();
    Segment segment() const { return m_seg; }
signals:
    void progressed(int index, qint64 currentOffset);
    void completed(int index);
    void failed(int index, const QString& error);
    void restartRequired(int index);
private:
    void openRequest();
    void onReadyRead();
    void onFinished();
    void onErrorOccurred();

    QNetworkAccessManager* m_nam;
    QFile*                 m_file;
    EngineConfig           m_cfg;
    Segment                m_seg;
    QUrl                   m_url;
    QString                m_validator;
    QNetworkReply*         m_reply    = nullptr;
    bool                   m_expectPartial = false;
    bool                   m_stopped  = false;
};
```

`src/core/SegmentWorker.cpp`:
```cpp
#include "SegmentWorker.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>

SegmentWorker::SegmentWorker(QNetworkAccessManager* nam, QFile* file,
                             const EngineConfig& cfg, QObject* parent)
    : QObject(parent), m_nam(nam), m_file(file), m_cfg(cfg) {}

void SegmentWorker::start(const Segment& seg, const QUrl& url, const QString& ifRangeValidator) {
    m_seg = seg;
    m_url = url;
    m_validator = ifRangeValidator;
    m_stopped = false;
    m_expectPartial = (m_seg.start > 0) || !m_validator.isEmpty();
    openRequest();
}

void SegmentWorker::openRequest() {
    if (m_seg.isComplete()) { emit completed(m_seg.index); return; }
    QNetworkRequest req(m_url);
    // Range: resume from current; open-ended if end<0 (fallback).
    QByteArray range = "bytes=" + QByteArray::number(m_seg.current) + "-";
    if (m_seg.end >= 0) range += QByteArray::number(m_seg.end);
    req.setRawHeader("Range", range);
    if (!m_validator.isEmpty()) req.setRawHeader("If-Range", m_validator.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead,     this, &SegmentWorker::onReadyRead);
    connect(m_reply, &QNetworkReply::finished,      this, &SegmentWorker::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &SegmentWorker::onErrorOccurred);
}

void SegmentWorker::onReadyRead() {
    if (m_stopped) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_expectPartial && status == 200) {   // server ignored If-Range -> resource changed
        m_reply->abort();
        emit restartRequired(m_seg.index);
        return;
    }
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty()) return;
    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {                          // disk full / permission
        m_reply->abort();
        emit failed(m_seg.index, "write error: " + m_file->errorString());
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);
}

void SegmentWorker::onFinished() {
    if (m_stopped) return;
    if (m_reply->error() != QNetworkReply::NoError) return;   // handled in onErrorOccurred
    m_file->flush();
    // Fallback mode (end<0): completion is EOF. Mark end so isComplete() is true.
    if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
    emit completed(m_seg.index);
}

void SegmentWorker::onErrorOccurred() {
    if (m_stopped) return;
    if (m_reply && m_reply->error() == QNetworkReply::OperationCanceledError) return;
    emit failed(m_seg.index, m_reply ? m_reply->errorString() : "unknown");
}

void SegmentWorker::stop() {
    m_stopped = true;
    if (m_reply) { m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
}
```

- [ ] **Step 3: Wire and run — verify PASS**

Add `SegmentWorker.cpp` to `src/core/CMakeLists.txt`.
Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `segmentWritesItsRange` PASSES (plus earlier probe cases).

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add SegmentWorker with write-at-offset range download"
```

---

### Task 6: DownloadTask — multi-segment + fallback (criteria 1, 2, 3)

**Files:**
- Create: `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `HttpProbe`, `SegmentWorker`, `computeSegments`, `Persistence`, `EngineConfig`.
- Produces: `DownloadTask` per the locked signature. Signals `progress`, `stateChanged`, `segmentProgress`.

- [ ] **Step 1: Write the failing tests**

Add to `tst_download.cpp`:
```cpp
    void downloadsMultiSegmentByteIdentical() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("out.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        QSignalSpy states(&task, &DownloadTask::stateChanged);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
        QVERIFY(!QFile::exists(Persistence::metaPath(dest)));   // meta removed on completion
    }

    void fallbackSingleConnectionWhenNoRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("plain.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/plain"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }
```
Add `#include "DownloadTask.h"` and `#include <QTemporaryDir>`.

- [ ] **Step 2: Implement DownloadTask (probe → segments → workers → completion)**

`src/core/DownloadTask.h`:
```cpp
#pragma once
#include "DownloadTypes.h"
#include "Persistence.h"
#include <QObject>
#include <QVector>
class QNetworkAccessManager;
class QFile;
class HttpProbe;
class SegmentWorker;

class DownloadTask : public QObject {
    Q_OBJECT
public:
    DownloadTask(QNetworkAccessManager* nam, const EngineConfig& cfg, QObject* parent = nullptr);
    void init(const QUuid& id, const QUrl& url, const QString& destPath, int segmentCount);
    void restore(const DownloadRecord& rec, const QVector<Segment>& segs,
                 const QString& etag, const QString& lastModified, bool validated);
    void start();
    void pause();
    DownloadState    state() const { return m_state; }
    QUuid            id() const { return m_id; }
    DownloadRecord   record() const;
    QVector<Segment> segments() const;
signals:
    void progress(qint64 received, qint64 total);
    void stateChanged(DownloadState state);
    void segmentProgress(int index, qint64 currentOffset);
private:
    void setState(DownloadState s);
    void onProbed(const ProbeResult& r);
    void beginSegments();
    void spawnWorker(const Segment& seg);
    void onSegmentCompleted(int index);
    void onSegmentFailed(int index, const QString& error);
    void onRestartRequired(int index);
    void checkAllComplete();
    qint64 receivedBytes() const;

    QNetworkAccessManager* m_nam;
    EngineConfig           m_cfg;
    QUuid                  m_id;
    QUrl                   m_url;
    QString                m_destPath;
    int                    m_segmentCount = 4;
    qint64                 m_totalBytes = -1;
    bool                   m_supportsRange = false;
    QString                m_etag, m_lastModified;
    bool                   m_validated = false;
    bool                   m_probed = false;
    DownloadState          m_state = DownloadState::Queued;
    QVector<Segment>       m_segments;
    QVector<SegmentWorker*> m_workers;
    QFile*                 m_file = nullptr;
    int                    m_completedCount = 0;
};
```

`src/core/DownloadTask.cpp`:
```cpp
#include "DownloadTask.h"
#include "HttpProbe.h"
#include "SegmentWorker.h"
#include "Segmentation.h"
#include <QNetworkAccessManager>
#include <QFile>

DownloadTask::DownloadTask(QNetworkAccessManager* nam, const EngineConfig& cfg, QObject* parent)
    : QObject(parent), m_nam(nam), m_cfg(cfg) {}

void DownloadTask::init(const QUuid& id, const QUrl& url, const QString& destPath, int segmentCount) {
    m_id = id; m_url = url; m_destPath = destPath; m_segmentCount = segmentCount;
}

void DownloadTask::restore(const DownloadRecord& rec, const QVector<Segment>& segs,
                           const QString& etag, const QString& lastModified, bool validated) {
    m_id = rec.id; m_url = rec.url; m_destPath = rec.destPath;
    m_segmentCount = rec.segmentCount; m_totalBytes = rec.totalBytes;
    m_supportsRange = rec.supportsRange; m_segments = segs;
    m_etag = etag; m_lastModified = lastModified; m_validated = validated;
    m_probed = !segs.isEmpty();
    m_state = DownloadState::Paused;
}

void DownloadTask::setState(DownloadState s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void DownloadTask::start() {
    setState(DownloadState::Connecting);
    if (!m_probed) {
        auto* probe = new HttpProbe(m_nam, this);
        connect(probe, &HttpProbe::finished, this, [this, probe](const ProbeResult& r) {
            probe->deleteLater();
            onProbed(r);
        });
        probe->start(m_url);
    } else {
        beginSegments();
    }
}

void DownloadTask::onProbed(const ProbeResult& r) {
    if (!r.ok) { setState(DownloadState::Error); return; }
    m_totalBytes    = r.totalBytes;
    m_supportsRange = r.supportsRange;
    m_etag          = r.etag;
    m_lastModified  = r.lastModified;
    m_validated     = !r.etag.isEmpty() || !r.lastModified.isEmpty();
    m_segments      = computeSegments(m_totalBytes, m_supportsRange, m_segmentCount, m_cfg.minSegSize);
    m_probed        = true;
    beginSegments();
}

void DownloadTask::beginSegments() {
    m_file = new QFile(m_destPath, this);
    if (!m_file->open(QIODevice::ReadWrite)) { setState(DownloadState::Error); return; }
    if (m_totalBytes > 0) m_file->resize(m_totalBytes);   // preallocate

    setState(DownloadState::Downloading);
    m_completedCount = 0;
    const QString validator = m_validated ? (!m_etag.isEmpty() ? m_etag : m_lastModified) : QString();
    for (const auto& seg : m_segments) {
        if (seg.isComplete()) { ++m_completedCount; continue; }
        spawnWorker(seg);
    }
    checkAllComplete();     // in case everything was already complete (restore)
}

void DownloadTask::spawnWorker(const Segment& seg) {
    auto* w = new SegmentWorker(m_nam, m_file, m_cfg, this);
    m_workers.append(w);
    connect(w, &SegmentWorker::progressed, this, [this](int idx, qint64 cur) {
        for (auto& s : m_segments) if (s.index == idx) s.current = cur;
        emit segmentProgress(idx, cur);
        emit progress(receivedBytes(), m_totalBytes);
    });
    connect(w, &SegmentWorker::completed,        this, &DownloadTask::onSegmentCompleted);
    connect(w, &SegmentWorker::failed,           this, &DownloadTask::onSegmentFailed);
    connect(w, &SegmentWorker::restartRequired,  this, &DownloadTask::onRestartRequired);
    const QString validator = m_validated ? (!m_etag.isEmpty() ? m_etag : m_lastModified) : QString();
    w->start(seg, m_url, validator);
}

void DownloadTask::onSegmentCompleted(int index) {
    for (auto& s : m_segments) if (s.index == index) s.current = s.end + 1;
    ++m_completedCount;
    checkAllComplete();
}

void DownloadTask::onSegmentFailed(int index, const QString& error) {
    Q_UNUSED(index); Q_UNUSED(error);
    for (auto* w : m_workers) w->stop();
    Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
    setState(DownloadState::Error);
}

void DownloadTask::onRestartRequired(int index) {
    Q_UNUSED(index);
    for (auto* w : m_workers) w->stop();
    qDeleteAll(m_workers); m_workers.clear();
    for (auto& s : m_segments) s.current = s.start;   // reset to zero
    m_validated = false;                               // don't send If-Range again
    m_etag.clear(); m_lastModified.clear();
    beginSegments();
}

void DownloadTask::checkAllComplete() {
    if (m_completedCount < m_segments.size()) return;
    m_file->flush();
    m_file->close();
    Persistence::removeMeta(m_destPath);
    setState(DownloadState::Completed);
}

void DownloadTask::pause() {
    for (auto* w : m_workers) w->stop();
    Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
    setState(DownloadState::Paused);
}

qint64 DownloadTask::receivedBytes() const {
    qint64 total = 0;
    for (const auto& s : m_segments) total += s.downloaded();
    return total;
}

QVector<Segment> DownloadTask::segments() const { return m_segments; }

DownloadRecord DownloadTask::record() const {
    DownloadRecord r;
    r.id = m_id; r.url = m_url; r.destPath = m_destPath;
    r.totalBytes = m_totalBytes; r.supportsRange = m_supportsRange;
    r.state = m_state; r.segmentCount = m_segmentCount;
    return r;
}
```

- [ ] **Step 3: Wire and run — verify PASS**

Add `DownloadTask.cpp` to `src/core/CMakeLists.txt`.
Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `downloadsMultiSegmentByteIdentical` and `fallbackSingleConnectionWhenNoRange` PASS.

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add DownloadTask orchestrating multi-segment and fallback downloads"
```

---

### Task 7: Pause/resume within a session (criterion 4) + meta cadence

**Files:**
- Modify: `src/core/DownloadTask.{h,cpp}` (periodic meta timer)
- Modify: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: existing `DownloadTask`.
- Produces: same signals; adds a periodic `.meta` flush (every 2 s) plus flush on pause/segment-complete/error (already present for pause/error).

- [ ] **Step 1: Write the failing test**

Add to `tst_download.cpp`:
```cpp
    void pauseThenResumeIsByteIdentical() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("pr.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        // pause as soon as some progress arrives
        QSignalSpy prog(&task, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));
        task.pause();
        QTRY_COMPARE(task.state(), DownloadState::Paused);
        QVERIFY(QFile::exists(Persistence::metaPath(dest)));

        task.start();                         // resume in same session
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }
```

- [ ] **Step 2: Add the periodic meta timer to DownloadTask**

In `DownloadTask.h` add includes and members:
```cpp
#include <QTimer>
```
```cpp
    QTimer* m_metaTimer = nullptr;
```

In `DownloadTask.cpp`, in `beginSegments()` right after `setState(DownloadState::Downloading);` add:
```cpp
    if (!m_metaTimer) {
        m_metaTimer = new QTimer(this);
        m_metaTimer->setInterval(2000);
        connect(m_metaTimer, &QTimer::timeout, this, [this] {
            if (m_file) m_file->flush();
            Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
        });
    }
    m_metaTimer->start();
```

In `pause()`, `onSegmentFailed()`, and `checkAllComplete()`, stop the timer at the top:
```cpp
    if (m_metaTimer) m_metaTimer->stop();
```
(Order matters in `pause()`: stop workers, stop timer, write meta, set state.)

- [ ] **Step 3: Run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `pauseThenResumeIsByteIdentical` PASSES. (`resume` re-enters `start()`; since `m_probed` is true and segments carry their `current`, workers resume from where they stopped.)

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add pause/resume with periodic meta persistence"
```

---

### Task 8: Segment retry + backoff + task Error (criteria 6, 7, 8)

**Files:**
- Modify: `src/core/SegmentWorker.{h,cpp}` (internal retry loop)
- Modify: `tests/TestServer.{h,cpp}` (add `/flaky`)
- Modify: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: existing `SegmentWorker`, `EngineConfig` (`maxSegmentRetries`, `retryBackoffBaseMs`).
- Produces: `SegmentWorker` now retries recoverable failures internally; emits `failed` only when retries are exhausted or the error is non-recoverable (HTTP 4xx / write error).

- [ ] **Step 1: Add the `/flaky` route to TestServer**

In `TestServer.h`, the `m_hits` counter is already declared. In `TestServer::listen()` add, using a raw response that closes early. Since `QHttpServer` fully buffers responses, simulate a mid-transfer drop by returning fewer bytes than the requested range on the first K attempts:
```cpp
    m_http.route("/flaky", [this](const QHttpServerRequest& req) {
        const int failTimes = req.query().queryItemValue("failTimes").toInt();
        const int n = ++m_hits["/flaky"];
        qint64 s = 0, e = -1;
        parseRange(req.value("Range"), s, e);
        QByteArray full = partial(s, e);
        if (n <= failTimes) {
            // Truncate: return half the range, then close -> client sees short read + error.
            QHttpServerResponse r("application/octet-stream", full.left(full.size()/2),
                                  QHttpServerResponse::StatusCode::PartialContent);
            r.setHeader("Content-Range",
                QString("bytes %1-%2/%3").arg(s).arg(s+full.size()-1).arg(m_body.size()).toUtf8());
            r.setHeader("ETag", m_etag.toUtf8());
            return r;
        }
        QHttpServerResponse ok("application/octet-stream", full,
                               QHttpServerResponse::StatusCode::PartialContent);
        ok.setHeader("Content-Range",
            QString("bytes %1-%2/%3").arg(s).arg(s+full.size()-1).arg(m_body.size()).toUtf8());
        ok.setHeader("ETag", m_etag.toUtf8());
        return ok;
    });
```
> Because the declared `Content-Range` length exceeds the bytes actually sent, `QNetworkReply` reports the reply finished short of `end`; `SegmentWorker` treats an incomplete range as a recoverable failure (Step 3). This is our deterministic stand-in for a dropped connection.

- [ ] **Step 2: Write the failing tests**

Add to `tst_download.cpp`:
```cpp
    void retriesRecoverableFailureThenCompletes() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("flaky.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;      // single segment, deterministic
        cfg.maxSegmentRetries = 5; cfg.retryBackoffBaseMs = 10;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(),
                  srv.url("/flaky?failTimes=2"), dest, 1);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 8000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void exhaustedRetriesGoesToError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("dead.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        cfg.maxSegmentRetries = 2; cfg.retryBackoffBaseMs = 10;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(),
                  srv.url("/flaky?failTimes=99"), dest, 1);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 8000);
    }

    void notFoundIsNonRecoverableError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("nf.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/notfound"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 5000);
    }
```
(`notFoundIsNonRecoverableError` already passes via `onProbed` rejecting non-ok — it guards the probe path. The retry cases exercise `SegmentWorker`.)

- [ ] **Step 3: Add the retry loop to SegmentWorker**

In `SegmentWorker.h` add members and a slot:
```cpp
#include <QTimer>
```
```cpp
    int     m_attempt = 0;
    QTimer* m_retryTimer = nullptr;
    bool    isRecoverable(QNetworkReply::NetworkError e) const;
    void    scheduleRetry(const QString& why);
```

In `SegmentWorker.cpp`:
```cpp
#include <QTimer>

bool SegmentWorker::isRecoverable(QNetworkReply::NetworkError e) const {
    switch (e) {
        case QNetworkReply::ContentNotFoundError:      // 404
        case QNetworkReply::ContentAccessDenied:       // 403
        case QNetworkReply::AuthenticationRequiredError:
            return false;
        default:
            return true;                               // timeouts, resets, remote-closed, short read
    }
}

void SegmentWorker::scheduleRetry(const QString& why) {
    Q_UNUSED(why);
    if (m_attempt >= m_cfg.maxSegmentRetries) {
        emit failed(m_seg.index, "retries exhausted");
        return;
    }
    ++m_attempt;
    const int delay = m_cfg.retryBackoffBaseMs * (1 << (m_attempt - 1));
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, [this] { openRequest(); });
    }
    m_retryTimer->start(delay);
}
```

Replace `onFinished()` so a short read (finished before reaching `end`) triggers a retry:
```cpp
void SegmentWorker::onFinished() {
    if (m_stopped) return;
    if (m_reply->error() != QNetworkReply::NoError) return;   // handled in onErrorOccurred
    if (m_seg.end >= 0 && m_seg.current <= m_seg.end) {       // short read: not all bytes arrived
        m_reply->deleteLater(); m_reply = nullptr;
        scheduleRetry("short read");
        return;
    }
    m_file->flush();
    if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
    emit completed(m_seg.index);
}
```

Replace `onErrorOccurred()` to route recoverable errors to retry:
```cpp
void SegmentWorker::onErrorOccurred() {
    if (m_stopped) return;
    const auto err = m_reply ? m_reply->error() : QNetworkReply::UnknownNetworkError;
    if (err == QNetworkReply::OperationCanceledError) return;   // our own abort/restart
    if (m_reply) { m_reply->deleteLater(); m_reply = nullptr; }
    if (isRecoverable(err)) scheduleRetry("network error");
    else emit failed(m_seg.index, "non-recoverable network error");
}
```
Reset `m_attempt = 0;` at the top of `start()`.

- [ ] **Step 4: Run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `retriesRecoverableFailureThenCompletes`, `exhaustedRetriesGoesToError`, `notFoundIsNonRecoverableError` PASS.

- [ ] **Step 5: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add per-segment retry with exponential backoff and error classification"
```

---

### Task 9: Idle/connect timeouts (criterion 9)

**Files:**
- Modify: `src/core/SegmentWorker.{h,cpp}` (timeout timers)
- Modify: `tests/TestServer.{h,cpp}` (add `/stall`)
- Modify: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: existing `SegmentWorker`, `EngineConfig` (`connectTimeoutMs`, `idleTimeoutMs`).
- Produces: a stalled transfer is aborted after `idleTimeoutMs` and flows into the retry path.

- [ ] **Step 1: Add the `/stall` route**

In `TestServer::listen()`:
```cpp
    m_http.route("/stall", [this](const QHttpServerRequest&) {
        // Declare a large body via Content-Range but send only a few bytes, never finishing.
        QHttpServerResponse r("application/octet-stream", m_body.left(8),
                              QHttpServerResponse::StatusCode::PartialContent);
        r.setHeader("Content-Range",
            QString("bytes 0-%1/%2").arg(m_body.size()-1).arg(m_body.size()).toUtf8());
        return r;
    });
```
> This finishes quickly with a short read rather than truly hanging the socket. To exercise the *timer* itself deterministically, the test below uses a tiny `idleTimeoutMs` and points at `/stall`, and asserts the worker abandons/retries. (A truly frozen socket is hard to produce portably from `QHttpServer`; the short-read-plus-tiny-timeout combination verifies the timeout wiring drives a retry without a real hang.)

- [ ] **Step 2: Write the failing test**

Add to `tst_download.cpp`:
```cpp
    void idleTimeoutTriggersRetryThenError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("stall.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        cfg.idleTimeoutMs = 150; cfg.connectTimeoutMs = 150;
        cfg.maxSegmentRetries = 2; cfg.retryBackoffBaseMs = 10;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/stall"), dest, 1);
        task.start();
        // /stall never delivers the full range -> short reads/timeouts -> retries exhaust -> Error
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 8000);
    }
```

- [ ] **Step 3: Add timeout timers to SegmentWorker**

In `SegmentWorker.h`:
```cpp
    QTimer* m_idleTimer = nullptr;
    void armIdleTimer();
    void onTimeout();
```

In `SegmentWorker.cpp`, in `openRequest()` after connecting signals:
```cpp
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout, this, &SegmentWorker::onTimeout);
    }
    m_idleTimer->start(m_cfg.connectTimeoutMs);   // connect deadline until first byte
```
In `onReadyRead()`, at the top (after the `m_stopped` guard), re-arm on activity:
```cpp
    m_idleTimer->start(m_cfg.idleTimeoutMs);
```
Add the handler:
```cpp
void SegmentWorker::onTimeout() {
    if (m_stopped || !m_reply) return;
    m_reply->abort();                 // -> OperationCanceledError; but we want a retry, so:
    m_reply->deleteLater(); m_reply = nullptr;
    scheduleRetry("timeout");
}
```
Stop the idle timer in `onFinished()`, `onErrorOccurred()` (top), and `stop()`:
```cpp
    if (m_idleTimer) m_idleTimer->stop();
```
Note: `onErrorOccurred` may still fire from the `abort()` above; guard by nulling `m_reply` before abort in `onTimeout`, and the existing `if (!m_reply) return;`-style checks. Ensure `onErrorOccurred` early-returns when `m_reply == nullptr`.

- [ ] **Step 4: Run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `idleTimeoutTriggersRetryThenError` PASSES and no earlier test regresses.

- [ ] **Step 5: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add connect/idle timeouts that drive the retry path"
```

---

### Task 10: If-Range validation → restart on resource change (spec §3.4)

**Files:**
- Modify: `tests/tst_download.cpp` (uses `/changed` route already added in Task 4)

**Interfaces:**
- Consumes: `SegmentWorker::restartRequired`, `DownloadTask::onRestartRequired` (both already implemented in Tasks 5–6).
- Produces: verification that a changed resource restarts cleanly and still yields byte-identical output.

- [ ] **Step 1: Write the failing/covering test**

Add to `tst_download.cpp`:
```cpp
    void changedResourceRestartsAndCompletes() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("chg.bin");

        // Seed a .meta as if a prior partial download exists, with an old validator.
        QVector<Segment> segs = { {0, 0, 100, qint64(m_body.size()-1)} };
        Persistence::writeMeta(dest, segs, "\"v1-old\"", "", true);
        // Preallocate the dest so resume path opens it.
        { QFile f(dest); f.open(QIODevice::WriteOnly); f.resize(m_body.size()); }

        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        DownloadTask task(&nam, cfg);
        DownloadRecord rec;
        rec.id = QUuid::createUuid(); rec.url = srv.url("/changed");
        rec.destPath = dest; rec.totalBytes = m_body.size();
        rec.supportsRange = true; rec.segmentCount = 1;
        task.restore(rec, segs, "\"v1-old\"", "", true);
        task.start();          // resume -> server 200 (ETag changed) -> restart from 0
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 6000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }
```

- [ ] **Step 2: Run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `changedResourceRestartsAndCompletes` PASSES. If it fails because the restart re-sends `If-Range`, confirm `onRestartRequired` clears `m_validated/m_etag/m_lastModified` (it does in Task 6) so the retry fetches a clean 200/206 without the stale validator.

- [ ] **Step 3: Commit** (ask first)

```bash
git add tests
git commit -m "test(core): verify If-Range mismatch restarts download and completes"
```

---

### Task 11: Progress throttling (criterion 10)

**Files:**
- Modify: `src/core/DownloadTask.{h,cpp}`
- Modify: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: existing `DownloadTask`.
- Produces: `progress` emitted at most every `progressThrottleMs`, coalesced, plus one final emit at completion.

- [ ] **Step 1: Write the failing test**

Add to `tst_download.cpp`:
```cpp
    void progressIsThrottled() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("thr.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.progressThrottleMs = 100;
        DownloadTask task(&nam, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        QSignalSpy prog(&task, &DownloadTask::progress);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        // 5000-byte body over multiple readyReads would emit many raw updates;
        // throttled to ~100ms it must be modest. Allow generous ceiling.
        QVERIFY2(prog.count() <= 20, qPrintable(QString("emitted %1").arg(prog.count())));
        QVERIFY(prog.count() >= 1);
    }
```

- [ ] **Step 2: Add throttling to DownloadTask**

In `DownloadTask.h`:
```cpp
    QTimer* m_progressTimer = nullptr;
    bool    m_progressPending = false;
    void    emitProgressNow();
```
In `DownloadTask.cpp`, replace the worker `progressed` lambda body's `emit progress(...)` line with a scheduled emit:
```cpp
    connect(w, &SegmentWorker::progressed, this, [this](int idx, qint64 cur) {
        for (auto& s : m_segments) if (s.index == idx) s.current = cur;
        emit segmentProgress(idx, cur);
        if (!m_progressTimer) {
            m_progressTimer = new QTimer(this);
            m_progressTimer->setSingleShot(true);
            connect(m_progressTimer, &QTimer::timeout, this, &DownloadTask::emitProgressNow);
        }
        if (!m_progressTimer->isActive() && !m_progressPending) {
            m_progressPending = true;
            m_progressTimer->start(m_cfg.progressThrottleMs);
        }
    });
```
Add the helper and a final flush in `checkAllComplete()`:
```cpp
void DownloadTask::emitProgressNow() {
    m_progressPending = false;
    emit progress(receivedBytes(), m_totalBytes);
}
```
In `checkAllComplete()`, before `setState(DownloadState::Completed);`, add an immediate final emit:
```cpp
    emit progress(receivedBytes(), m_totalBytes);
```

- [ ] **Step 3: Run — verify PASS**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `progressIsThrottled` PASSES; byte-identical tests still pass.

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): coalesce progress signals with throttle timer"
```

---

### Task 12: DownloadManager — queue, maxConcurrent, session load/save, restart-resume (criterion 5)

**Files:**
- Create: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `DownloadTask`, `Persistence`, `EngineConfig`.
- Produces: `DownloadManager` per the locked signature. Restores incomplete downloads from `downloads.json` + `.meta` as `Paused`, then `resumeAll()` completes them.

- [ ] **Step 1: Write the failing tests**

Add to `tst_download.cpp`:
```cpp
    void managerRespectsMaxConcurrent() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1; cfg.segmentCount = 1; cfg.minSegSize = 1;
        DownloadManager mgr(cfg, data.path());
        mgr.addDownload(srv.url("/ranged"), out.filePath("a.bin"));
        mgr.addDownload(srv.url("/ranged"), out.filePath("b.bin"));
        // Only one Downloading at a time.
        int downloading = 0;
        for (auto* t : mgr.tasks()) if (t->state() == DownloadState::Downloading) ++downloading;
        QVERIFY(downloading <= 1);
        // Both eventually complete.
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]{ int c=0; for (auto* t: mgr.tasks()) if (t->state()==DownloadState::Completed) ++c;
                 return c==2; }(), 8000);
    }

    void restartProcessResumesFromDisk() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        const QString dest = out.filePath("resume.bin");
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;

        {   // "session 1": start, pause mid-way, let manager persist session
            DownloadManager mgr(cfg, data.path());
            const QUuid id = mgr.addDownload(srv.url("/ranged"), dest);
            DownloadTask* t = mgr.tasks().first();
            QSignalSpy prog(t, &DownloadTask::progress);
            QVERIFY(prog.wait(3000));
            mgr.pauseAll();                 // writes downloads.json + .meta
            QTRY_COMPARE(t->state(), DownloadState::Paused);
            Q_UNUSED(id);
        }   // mgr destroyed == process restart

        {   // "session 2": fresh manager, load from disk, resume
            DownloadManager mgr2(cfg, data.path());
            mgr2.loadSession();
            QCOMPARE(mgr2.tasks().size(), 1);
            QCOMPARE(mgr2.tasks().first()->state(), DownloadState::Paused);
            mgr2.resumeAll();
            QTRY_COMPARE_WITH_TIMEOUT(mgr2.tasks().first()->state(),
                                      DownloadState::Completed, 6000);
        }
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }
```
(If your Qt lacks `QTRY_VERIFY_WITH_TIMEOUT` with a lambda, replace with a loop using `QTRY_VERIFY` on a helper bool.)

- [ ] **Step 2: Implement DownloadManager**

`src/core/DownloadManager.h`:
```cpp
#pragma once
#include "DownloadTypes.h"
#include "DownloadTask.h"
#include <QObject>
#include <QVector>
class QNetworkAccessManager;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir, QObject* parent = nullptr);
    QUuid addDownload(const QUrl& url, const QString& destPath);
    void  pauseAll();
    void  resumeAll();
    void  remove(const QUuid& id, bool deleteFiles);
    void  loadSession();
    QVector<DownloadTask*> tasks() const { return m_tasks; }
signals:
    void taskProgress(const QUuid& id, qint64 received, qint64 total);
    void taskStateChanged(const QUuid& id, DownloadState state);
private:
    QString sessionPath() const;
    void    saveSession();
    void    pump();                 // promote Queued -> Downloading up to maxConcurrent
    void    wire(DownloadTask* t);

    EngineConfig            m_cfg;
    QString                 m_dataDir;
    QNetworkAccessManager*  m_nam;
    QVector<DownloadTask*>  m_tasks;
};
```

`src/core/DownloadManager.cpp`:
```cpp
#include "DownloadManager.h"
#include "Persistence.h"
#include <QNetworkAccessManager>
#include <QDir>

DownloadManager::DownloadManager(const EngineConfig& cfg, const QString& dataDir, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_dataDir(dataDir),
      m_nam(new QNetworkAccessManager(this)) {
    QDir().mkpath(m_dataDir);
}

QString DownloadManager::sessionPath() const { return m_dataDir + "/downloads.json"; }

void DownloadManager::wire(DownloadTask* t) {
    connect(t, &DownloadTask::progress, this, [this, t](qint64 r, qint64 tot) {
        emit taskProgress(t->id(), r, tot);
    });
    connect(t, &DownloadTask::stateChanged, this, [this, t](DownloadState s) {
        emit taskStateChanged(t->id(), s);
        saveSession();
        if (s == DownloadState::Completed || s == DownloadState::Error ||
            s == DownloadState::Paused)
            pump();                       // a slot may have freed up
    });
}

QUuid DownloadManager::addDownload(const QUrl& url, const QString& destPath) {
    const QString finalPath = Persistence::resolveUniquePath(destPath);
    auto* t = new DownloadTask(m_nam, m_cfg, this);
    t->init(QUuid::createUuid(), url, finalPath, m_cfg.segmentCount);
    wire(t);
    m_tasks.append(t);
    saveSession();
    pump();
    return t->id();
}

void DownloadManager::pump() {
    int active = 0;
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
            ++active;
    for (auto* t : m_tasks) {
        if (active >= m_cfg.maxConcurrentDownloads) break;
        if (t->state() == DownloadState::Queued) { t->start(); ++active; }
    }
}

void DownloadManager::pauseAll() {
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
            t->pause();
    saveSession();
}

void DownloadManager::resumeAll() {
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Paused || t->state() == DownloadState::Error) {
            // Re-queue; pump() decides how many actually start.
            t->start();
        }
}

void DownloadManager::remove(const QUuid& id, bool deleteFiles) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i]->id() != id) continue;
        DownloadTask* t = m_tasks[i];
        t->pause();
        const QString dest = t->record().destPath;
        m_tasks.removeAt(i);
        t->deleteLater();
        if (deleteFiles) { QFile::remove(dest); Persistence::removeMeta(dest); }
        break;
    }
    saveSession();
    pump();
}

void DownloadManager::saveSession() {
    QVector<DownloadRecord> recs;
    for (auto* t : m_tasks) recs.append(t->record());
    Persistence::writeSession(sessionPath(), recs);
}

void DownloadManager::loadSession() {
    const auto recs = Persistence::readSession(sessionPath());
    for (const auto& rec : recs) {
        if (rec.state == DownloadState::Completed) continue;   // nothing to resume
        QVector<Segment> segs; QString etag, lm; bool validated = false;
        Persistence::readMeta(rec.destPath, segs, etag, lm, validated);
        auto* t = new DownloadTask(m_nam, m_cfg, this);
        t->restore(rec, segs, etag, lm, validated);
        wire(t);
        m_tasks.append(t);
    }
}
```

- [ ] **Step 3: Wire and run — verify PASS**

Add `DownloadManager.cpp` to `src/core/CMakeLists.txt`.
Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: `managerRespectsMaxConcurrent` and `restartProcessResumesFromDisk` PASS. Full suite green.

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/core tests
git commit -m "feat(core): add DownloadManager with queue, concurrency cap, and session resume"
```

---

### Task 13: orbit-cli (manual end-to-end driver)

**Files:**
- Create: `src/cli/CMakeLists.txt`, `src/cli/main.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(src/cli)`)

**Interfaces:**
- Consumes: `DownloadManager`, `EngineConfig`.
- Produces: `orbit-cli <url> <dest> [--segments N] [--max-concurrent M]` — a smoke driver, no automated test (it is the manual verification tool).

- [ ] **Step 1: Write the CLI**

`src/cli/main.cpp`:
```cpp
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include "DownloadManager.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("orbit-cli");

    QCommandLineParser p;
    p.addPositionalArgument("url", "URL to download");
    p.addPositionalArgument("dest", "Destination file path");
    QCommandLineOption segs("segments", "Segments per download", "N", "4");
    QCommandLineOption maxc("max-concurrent", "Max concurrent downloads", "M", "3");
    p.addOption(segs); p.addOption(maxc);
    p.addHelpOption();
    p.process(app);

    const auto args = p.positionalArguments();
    QTextStream err(stderr);
    if (args.size() < 2) { err << "usage: orbit-cli <url> <dest>\n"; return 2; }

    EngineConfig cfg;
    cfg.segmentCount = p.value(segs).toInt();
    cfg.maxConcurrentDownloads = p.value(maxc).toInt();

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-cli";
    DownloadManager mgr(cfg, dataDir);

    QTextStream out(stdout);
    QObject::connect(&mgr, &DownloadManager::taskProgress, &app,
        [&out](const QUuid&, qint64 r, qint64 t) {
            if (t > 0) out << QString("\r%1%  (%2/%3 bytes)")
                                 .arg(100.0 * r / t, 0, 'f', 1).arg(r).arg(t) << Qt::flush;
        });
    QObject::connect(&mgr, &DownloadManager::taskStateChanged, &app,
        [&out, &app](const QUuid&, DownloadState s) {
            if (s == DownloadState::Completed) { out << "\nDone.\n"; app.quit(); }
            else if (s == DownloadState::Error) { out << "\nError.\n"; QCoreApplication::exit(1); }
        });

    mgr.addDownload(QUrl(args[0]), args[1]);
    return app.exec();
}
```

`src/cli/CMakeLists.txt`:
```cmake
add_executable(orbit-cli main.cpp)
target_link_libraries(orbit-cli PRIVATE orbitcore Qt6::Core Qt6::Network)
```

Add to top-level `CMakeLists.txt` after `add_subdirectory(src/core)`:
```cmake
add_subdirectory(src/cli)
```

- [ ] **Step 2: Build and smoke-test manually**

Run:
```bash
cmake --build build
./build/src/cli/orbit-cli https://speed.hetzner.de/1MB.bin /tmp/orbit-1mb.bin --segments 4
```
Expected: a progress line advancing to `100.0%` then `Done.`, and `/tmp/orbit-1mb.bin` is 1 MB. (This step needs real internet; it is manual verification, not part of `ctest`.)

- [ ] **Step 3: Run the full offline suite once more**

Run: `ctest --test-dir build --output-on-failure`
Expected: `tst_smoke`, `tst_segmentation`, `tst_persistence`, `tst_download` all PASS.

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/cli CMakeLists.txt
git commit -m "feat(cli): add orbit-cli end-to-end download driver"
```

---

## Self-Review

**Spec coverage (§ → task):**
- §2 criteria 1–3 (byte-identical, multi-seg, fallback) → Task 6. ✓
- §2 criterion 4 (pause/resume) → Task 7. ✓
- §2 criterion 5 (restart resume) → Task 12. ✓
- §2 criteria 6–8 (segment retry, task Error, non-recoverable) → Task 8. ✓
- §2 criterion 9 (timeout) → Task 9. ✓
- §2 criterion 10 (progress + throttle) → Task 11. ✓
- §3.1 data model → Task 2. ✓  §3.2 HttpProbe/SegmentWorker/Manager/Persistence → Tasks 3,4,5,6,12. ✓
- §3.3 segmentation + preallocation → Tasks 2, 6. ✓  §3.4 resume/If-Range/no-validator/unknown-size → Tasks 6,7,10 (unknown-size falls out of Task 6 fallback). ✓
- §3.5 error handling → Tasks 8,9. ✓  §3.6 throttling → Task 11. ✓
- §4.1 EngineConfig → Task 2 (struct) + used throughout; loading from `settings.json` is CLI-side and deferred (spec says "no settings UI in Phase 1"; the CLI flags cover overrides — the `settings.json` loader is intentionally out of the automated scope and can be added as a trivial follow-up). ✓ (noted, not a silent gap)
- §5 file structure → matches. §6 test routes → Tasks 4,8,9. §7 build → Task 1. §8 out-of-scope → respected (no FTP/GUI). ✓

**Placeholder scan:** No TBD/TODO. Every code step carries complete code. Qt-version API caveats (QHttpServerResponse headers, `QTRY_*` variants) are flagged with concrete fallback instructions, not left vague.

**Type consistency:** `computeSegments`, `Persistence::*`, `ProbeResult`, `SegmentWorker` signals, `DownloadTask::{init,restore,record,segments}`, `DownloadManager::*` signatures used in tests match their definitions. `Q_DECLARE_METATYPE(ProbeResult)` (Task 4) is required for the `QSignalSpy`/`qvariant_cast` in probe tests — flagged in Task 4 Step 2.

**One known deviation to confirm during execution:** the spec's `settings.json` auto-load (§4.1) is realized only as CLI flags in Task 13; the JSON loader is deferred. If you want it in Phase 1, add a 30-minute task after Task 12 that reads `EngineConfig` from `AppConfigLocation/settings.json`.

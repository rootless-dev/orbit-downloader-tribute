# GUI base (Fase 2) — Implementation Plan

**Goal:** Build a QtWidgets GUI (`orbit-gui`) with the classic Orbit layout — toolbar, category tree, downloads table, and Log/Progress/Properties tabs including the dense colored-block segment grid — driving the Fase 1 Core through its existing signals.

**Architecture:** The Core (`orbitcore`) stays QtWidgets-free; the only Core change is extending `DownloadManager` with per-id `pause`/`resume`/`taskById`, routed through the existing `pump()`/concurrency cap. All non-visual logic (segment→cell geometry, speed/ETA, extension→category, URL→filename) is extracted into pure units in a `orbitgui_logic` static lib, unit-tested headless. Widgets and the `QAbstractTableModel` live in a `orbitgui` static lib; `orbit-gui` is a thin `main`. Tests run under `QT_QPA_PLATFORM=offscreen`.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, HttpServer, Test), CMake, QtTest.

## Global Constraints

- **Qt version floor:** Qt 6.11 (Homebrew, `CMAKE_PREFIX_PATH=/opt/homebrew`). C++ standard: C++20 (`CMAKE_CXX_STANDARD 20`, required, no compiler extensions).
- **Core stays QtWidgets-free.** `orbitcore` links only `Qt6::Core` and `Qt6::Network`. Only `src/gui/**` may link `Qt6::Widgets`.
- **GUI never calls `DownloadTask::start()/pause()/requeue()` directly.** All per-item control goes through `DownloadManager::pause(id)/resume(id)` so the concurrency cap (`maxConcurrentDownloads`) is respected — the same invariant that produced the Fase 1 CRITICAL fix.
- **Pure units carry no QtWidgets.** `GridGeometry`, `SpeedSampler`, `FileType`, `deriveFileName` link `Qt6::Core` only and live in `orbitgui_logic`.
- **All tests are offline** — any network test hits `127.0.0.1` via the in-process `TestServer` (Fase 1) only. GUI tests run headless with `QT_QPA_PLATFORM=offscreen`.
- **Concurrency model:** single-threaded, async on the main event loop. No `QThread`, no mutexes.
- **Commit discipline:** conventional-commit messages **in English**, no co-author trailer.
- **Time is injected into `SpeedSampler`** (caller passes a monotonic `tMs`); pure units never read a clock, `Date`, or randomness.

---

## File Structure

```
orbit-downloader-tribute/
  CMakeLists.txt                 # + Widgets component; add_subdirectory(src/gui)
  src/
    core/
      DownloadManager.{h,cpp}    # MODIFY: + taskById/pause/resume
    gui/
      CMakeLists.txt             # orbitgui_logic (pure) + orbitgui (widgets) + orbit-gui exe
      FileType.{h,cpp}           # pure: extension -> Category  (orbitgui_logic)
      UrlName.{h,cpp}            # pure: deriveFileName(QUrl)    (orbitgui_logic)
      GridGeometry.{h,cpp}       # pure: (bytes,segments,state,nCells) -> QVector<Cell>  (orbitgui_logic)
      SpeedSampler.{h,cpp}       # pure: samples -> bytes/s, ETA  (orbitgui_logic)
      DownloadTableModel.{h,cpp} # QAbstractTableModel over DownloadManager  (orbitgui)
      CategoryFilterProxy.{h,cpp}# QSortFilterProxyModel by state/category   (orbitgui)
      ProgressGridWidget.{h,cpp} # QWidget painting the dense grid           (orbitgui)
      NewDownloadDialog.{h,cpp}  # QDialog: URL + folder                     (orbitgui)
      CategoryTree.{h,cpp}       # QTreeWidget emitting the active filter     (orbitgui)
      MainWindow.{h,cpp}         # QMainWindow assembly + wiring + Log/Props  (orbitgui)
      main_gui.cpp               # QApplication entry point                   (orbit-gui exe)
  tests/
    CMakeLists.txt               # + tst_gui target (offscreen), + manager-extension cases
    tst_gui.cpp                  # QtTest headless: pure units, model, proxy, widget smoke
    tst_download.cpp             # MODIFY: + pause/resume/taskById cases (reuses TestServer)
  docs/design/{specs,plans}/
```

Each file has one responsibility. Pure logic (`FileType`, `UrlName`, `GridGeometry`, `SpeedSampler`) is unit-tested with no widgets. Widgets get construction/behavior smoke tests; full visual behavior is validated manually (Task 13).

## Interfaces (locked signatures)

Defined by the task noted; later tasks consume them verbatim.

```cpp
// FileType.h  (Task 3)  — orbitgui_logic, QtCore only
namespace FileType {
    enum class Category { Movie, Software, Music, Others };
    Category categorize(const QString& fileName);   // by extension, case-insensitive
    QString  displayName(Category c);               // "Movie" | "Software" | "Music" | "Others"
}

// UrlName.h  (Task 4)  — orbitgui_logic
QString deriveFileName(const QUrl& url);            // last path segment, percent-decoded; "download" if empty

// GridGeometry.h  (Task 5)  — orbitgui_logic
enum class CellKind { Pending, Downloaded, Error };
struct Cell {
    CellKind kind         = CellKind::Pending;
    int      segmentIndex = -1;                     // set only when kind == Downloaded
};
QVector<Cell> computeCells(qint64 totalBytes,
                           const QVector<Segment>& segments,
                           DownloadState state,
                           int nCells);             // nCells<=0 or totalBytes<=0 -> all Pending

// SpeedSampler.h  (Task 6)  — orbitgui_logic
class SpeedSampler {
public:
    void   addSample(qint64 bytes, qint64 tMs);     // monotonic tMs injected by caller
    double bytesPerSec() const;                     // 0.0 if <2 samples in window or no progress
    qint64 etaSeconds(qint64 totalBytes) const;     // -1 if speed~0 or totalBytes<=0
    void   reset();
private:
    struct Sample { qint64 bytes; qint64 tMs; };
    QVector<Sample> m_samples;                       // trimmed to a ~5000 ms window
};

// DownloadManager.h  (Task 2)  — orbitcore
DownloadTask* taskById(const QUuid& id) const;       // nullptr if absent
void          pause(const QUuid& id);                // active/queued -> task->pause()
void          resume(const QUuid& id);               // paused/error  -> task->requeue(); pump()

// DownloadTableModel.h  (Task 7)  — orbitgui
class DownloadTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Progress, Status, Speed, TimeLeft, ColumnCount };
    enum Roles  { StateRole = Qt::UserRole + 1, CategoryRole, ProgressRole, TaskRole };
    explicit DownloadTableModel(DownloadManager* mgr, QObject* parent = nullptr);
    int      rowCount(const QModelIndex& = {}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int, Qt::Orientation, int) const override;
    void          appendTask(DownloadTask* t);       // beginInsertRows
    void          removeTaskById(const QUuid& id);   // beginRemoveRows
    DownloadTask* taskAt(int row) const;
};

// CategoryFilterProxy.h  (Task 8)  — orbitgui
class CategoryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class Filter { All, Downloading, Completed, Movie, Software, Music, Others };
    void setFilter(Filter f);
protected:
    bool filterAcceptsRow(int srcRow, const QModelIndex& srcParent) const override;
};

// ProgressGridWidget.h  (Task 9)  — orbitgui
class ProgressGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProgressGridWidget(QWidget* parent = nullptr);
    void setTask(DownloadTask* t);                   // disconnect old, connect segmentProgress
protected:
    void paintEvent(QPaintEvent*) override;
};

// NewDownloadDialog.h  (Task 10)  — orbitgui
class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDownloadDialog(QWidget* parent = nullptr);
    QUrl        url() const;
    QString     destPath() const;                    // chosenDir + "/" + deriveFileName(url())
    static bool isValidHttpUrl(const QUrl& u);        // non-empty, scheme http/https
};

// CategoryTree.h  (Task 11)  — orbitgui
class CategoryTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit CategoryTree(QWidget* parent = nullptr);
signals:
    void filterChanged(CategoryFilterProxy::Filter f);
};

// MainWindow.h  (Task 12)  — orbitgui
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(DownloadManager* mgr, DownloadTableModel* model, QWidget* parent = nullptr);
};
```

---

### Task 1: GUI build scaffolding + empty app

**Files:**
- Modify: `CMakeLists.txt` (root — add `Widgets`, `add_subdirectory(src/gui)`)
- Create: `src/gui/CMakeLists.txt`
- Create: `src/gui/main_gui.cpp` (temporary empty window)
- Modify: `tests/CMakeLists.txt` (add `tst_gui` target, offscreen env)
- Create: `tests/tst_gui.cpp` (one placeholder test)

**Interfaces:**
- Consumes: `orbitcore` target (Fase 1).
- Produces: build targets `orbitgui_logic`, `orbitgui`, `orbit-gui`, and test `tst_gui`.

- [ ] **Step 1: Add Widgets to the root CMake and pull in the gui subdir**

In `CMakeLists.txt` (root), add `Widgets` to the Qt components and add the subdirectory after the core/cli ones:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets HttpServer Test)
# ... existing add_subdirectory(src/core) etc ...
add_subdirectory(src/gui)
```

- [ ] **Step 2: Create `src/gui/CMakeLists.txt` with the three targets**

Match the repo's existing CMake idiom (plain `add_library`/`add_executable`; `CMAKE_AUTOMOC` is already `ON` globally in the root — no `qt_standard_project_setup`/`qt_add_executable`):

```cmake
# Pure logic (no QtWidgets) — testable headless
add_library(orbitgui_logic STATIC
    FileType.cpp
    UrlName.cpp
    GridGeometry.cpp
    SpeedSampler.cpp
)
target_link_libraries(orbitgui_logic PUBLIC orbitcore Qt6::Core)
target_include_directories(orbitgui_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Widgets + model
add_library(orbitgui STATIC
    DownloadTableModel.cpp
    CategoryFilterProxy.cpp
    ProgressGridWidget.cpp
    NewDownloadDialog.cpp
    CategoryTree.cpp
    MainWindow.cpp
)
target_link_libraries(orbitgui PUBLIC orbitgui_logic orbitcore Qt6::Widgets Qt6::Core)
target_include_directories(orbitgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Thin executable
add_executable(orbit-gui main_gui.cpp)
target_link_libraries(orbit-gui PRIVATE orbitgui)
```

Note: the `orbitgui_logic` and `orbitgui` source files listed above are filled by later tasks. For this task, create **minimal stub .cpp/.h pairs** for each listed source so both libs archive and `orbit-gui` links:
- Each stub `.h`: `#pragma once` + an empty class declaration (e.g. `class FileType;`-style is not enough — for the widget classes give an empty `class Foo : public QWidget { Q_OBJECT public: explicit Foo(QWidget* =nullptr){} };`, for pure headers just an include guard).
- Each stub `.cpp`: only `#include "Foo.h"`.
- The Task 1 `main_gui.cpp` (empty window) does **not** reference any stub symbol, so undefined methods in the stubs never link — the static libs build fine. Later tasks **replace both files** of a unit with the real locked-interface content, in the task order below (each unit's dependencies are created before it).

Keep stubs trivial; the point is only that Task 1 produces a buildable tree and a green placeholder test.

- [ ] **Step 3: Write the temporary empty window `src/gui/main_gui.cpp`**

```cpp
#include <QApplication>
#include <QMainWindow>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QMainWindow w;
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(900, 600);
    w.show();
    return app.exec();
}
```

- [ ] **Step 4: Add the `tst_gui` target in `tests/CMakeLists.txt`**

```cmake
add_executable(tst_gui tst_gui.cpp)
target_link_libraries(tst_gui PRIVATE orbitgui Qt6::Test Qt6::HttpServer)
add_test(NAME tst_gui COMMAND tst_gui)
set_tests_properties(tst_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

(`Qt6::HttpServer` is linked because Task 7 adds `TestServer.cpp` — which uses `QHttpServer` — to this target, exactly like `tst_download`.)

- [ ] **Step 5: Write the placeholder test `tests/tst_gui.cpp`**

```cpp
#include <QtTest>

class TestGui : public QObject {
    Q_OBJECT
private slots:
    void scaffolding_builds() { QVERIFY(true); }
};

QTEST_MAIN(TestGui)
#include "tst_gui.moc"
```

- [ ] **Step 6: Configure, build, and run**

Run:
```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build
ctest --test-dir build --output-on-failure -R tst_gui
```
Expected: `orbit-gui` builds; `tst_gui` passes (1 test).

- [ ] **Step 7: Commit** (ask the human first)

```bash
git add CMakeLists.txt src/gui tests/CMakeLists.txt tests/tst_gui.cpp
git commit -m "build(gui): scaffold orbit-gui targets and headless test harness"
```

---

### Task 2: Core — per-id pause/resume/taskById on DownloadManager

**Files:**
- Modify: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`
- Test: `tests/tst_download.cpp` (add cases)

**Interfaces:**
- Consumes: existing `m_tasks`, `pump()`, `DownloadTask::pause()/requeue()/state()/id()`.
- Produces: `taskById(id)`, `pause(id)`, `resume(id)` (signatures in the locked block).

- [ ] **Step 1: Write the failing tests**

Add to `tests/tst_download.cpp` (use the existing `TestServer` + temp-dir helpers already in the file). Assumes a `/ranged` route serving a known N-byte body.

```cpp
void pause_then_resume_via_manager_completes() {
    TestServer srv; srv.start();
    EngineConfig cfg; cfg.segmentCount = 4; cfg.maxConcurrentDownloads = 3;
    QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
    QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 3000));

    mgr.pause(id);
    QVERIFY(waitForState(mgr, id, DownloadState::Paused, 3000));

    mgr.resume(id);
    QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
    QCOMPARE(readFile(dir + "/out.bin"), srv.body("/ranged"));
}

void resume_respects_concurrency_cap() {
    TestServer srv; srv.start();
    EngineConfig cfg; cfg.maxConcurrentDownloads = 2;
    QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    QVector<QUuid> ids;
    for (int i = 0; i < 3; ++i)
        ids << mgr.addDownload(srv.url("/ranged"), dir + QString("/o%1.bin").arg(i));
    // With cap=2, at most 2 are Downloading at once.
    QTRY_VERIFY(countInState(mgr, DownloadState::Downloading) <= 2);
    mgr.pauseAll();
    QTRY_COMPARE(countInState(mgr, DownloadState::Downloading), 0);
    for (auto id : ids) mgr.resume(id);
    // Resuming all three must NOT exceed the cap.
    QTRY_VERIFY(countInState(mgr, DownloadState::Downloading) <= 2);
}

void pause_resume_unknown_id_is_noop() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    mgr.pause(QUuid::createUuid());   // must not crash
    mgr.resume(QUuid::createUuid());  // must not crash
    QVERIFY(true);
}
```

If `countInState`/`waitForState` helpers do not already exist in the file, add small local helpers that iterate `mgr.tasks()` and check `t->state()`.

- [ ] **Step 2: Run to verify failure**

Run: `ctest --test-dir build --output-on-failure -R tst_download`
Expected: FAIL — `pause`/`resume`/`taskById` not declared.

- [ ] **Step 3: Declare the methods in `DownloadManager.h`**

Add under the existing public methods:
```cpp
    DownloadTask* taskById(const QUuid& id) const;
    void          pause(const QUuid& id);
    void          resume(const QUuid& id);
```

- [ ] **Step 4: Implement in `DownloadManager.cpp`**

```cpp
DownloadTask* DownloadManager::taskById(const QUuid& id) const {
    for (DownloadTask* t : m_tasks)
        if (t->id() == id) return t;
    return nullptr;
}

void DownloadManager::pause(const QUuid& id) {
    DownloadTask* t = taskById(id);
    if (!t) return;
    switch (t->state()) {
        case DownloadState::Queued:
        case DownloadState::Connecting:
        case DownloadState::Downloading:
            t->pause();
            break;
        default: break;   // Paused/Completed/Error: no-op
    }
    saveSession();
}

void DownloadManager::resume(const QUuid& id) {
    DownloadTask* t = taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Paused || t->state() == DownloadState::Error) {
        t->requeue();     // -> Queued
        pump();           // promotes Queued -> Downloading only up to the cap
    }
}
```

(If `requeue()` already resets to `Queued`, do not also call `start()` — `pump()` owns promotion.)

- [ ] **Step 5: Run to verify pass**

Run: `ctest --test-dir build --output-on-failure -R tst_download`
Expected: PASS (all cases, including the 3 new ones).

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/core/DownloadManager.h src/core/DownloadManager.cpp tests/tst_download.cpp
git commit -m "feat(core): add per-id pause/resume/taskById routed through the concurrency cap"
```

---

### Task 3: FileType (pure) — extension → category

**Files:**
- Create: `src/gui/FileType.h`, `src/gui/FileType.cpp` (replace the Task 1 stub)
- Test: `tests/tst_gui.cpp` (add cases)

**Interfaces:**
- Produces: `FileType::Category`, `FileType::categorize`, `FileType::displayName` (locked block).

- [ ] **Step 1: Write the failing tests** (add slots to `TestGui`)

```cpp
void filetype_categorizes_by_extension() {
    using namespace FileType;
    QCOMPARE(categorize("movie.MP4"),   Category::Movie);
    QCOMPARE(categorize("clip.mkv"),    Category::Movie);
    QCOMPARE(categorize("setup.dmg"),   Category::Software);
    QCOMPARE(categorize("app.zip"),     Category::Software);
    QCOMPARE(categorize("song.flac"),   Category::Music);
    QCOMPARE(categorize("track.mp3"),   Category::Music);
    QCOMPARE(categorize("notes.txt"),   Category::Others);
    QCOMPARE(categorize("noext"),       Category::Others);
    QCOMPARE(categorize(""),            Category::Others);
}
void filetype_display_names() {
    using namespace FileType;
    QCOMPARE(displayName(Category::Movie),  QString("Movie"));
    QCOMPARE(displayName(Category::Others), QString("Others"));
}
```

Add `#include "FileType.h"` at the top of `tst_gui.cpp`.

- [ ] **Step 2: Run to verify failure**

Run: `ctest --test-dir build --output-on-failure -R tst_gui`
Expected: FAIL — undefined symbols.

- [ ] **Step 3: Write `FileType.h`**

```cpp
#pragma once
#include <QString>
namespace FileType {
    enum class Category { Movie, Software, Music, Others };
    Category categorize(const QString& fileName);
    QString  displayName(Category c);
}
```

- [ ] **Step 4: Write `FileType.cpp`**

```cpp
#include "FileType.h"
#include <QFileInfo>
#include <QSet>

namespace {
    const QSet<QString> kMovie    = {"mp4","mkv","avi","mov","wmv","flv","webm","m4v","mpg","mpeg"};
    const QSet<QString> kSoftware = {"dmg","pkg","exe","msi","deb","rpm","appimage","app",
                                     "zip","7z","rar","tar","gz","iso"};
    const QSet<QString> kMusic    = {"mp3","flac","aac","wav","ogg","m4a","wma","opus"};
}

namespace FileType {
Category categorize(const QString& fileName) {
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (kMovie.contains(ext))    return Category::Movie;
    if (kSoftware.contains(ext)) return Category::Software;
    if (kMusic.contains(ext))    return Category::Music;
    return Category::Others;
}
QString displayName(Category c) {
    switch (c) {
        case Category::Movie:    return "Movie";
        case Category::Software: return "Software";
        case Category::Music:    return "Music";
        case Category::Others:   return "Others";
    }
    return "Others";
}
}
```

- [ ] **Step 5: Run to verify pass**

Run: `ctest --test-dir build --output-on-failure -R tst_gui`
Expected: PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/FileType.h src/gui/FileType.cpp tests/tst_gui.cpp
git commit -m "feat(gui): add FileType extension-to-category classifier"
```

---

### Task 4: UrlName (pure) — deriveFileName

**Files:**
- Create: `src/gui/UrlName.h`, `src/gui/UrlName.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Produces: `QString deriveFileName(const QUrl&)`.

- [ ] **Step 1: Write the failing tests**

```cpp
void urlname_derives_filename() {
    QCOMPARE(deriveFileName(QUrl("https://x.com/a/b/file.zip")),      QString("file.zip"));
    QCOMPARE(deriveFileName(QUrl("https://x.com/file.zip?k=v&x=1")),  QString("file.zip"));
    QCOMPARE(deriveFileName(QUrl("https://x.com/path/my%20doc.pdf")), QString("my doc.pdf"));
    QCOMPARE(deriveFileName(QUrl("https://x.com/")),                  QString("download"));
    QCOMPARE(deriveFileName(QUrl("https://x.com")),                   QString("download"));
}
```

Add `#include "UrlName.h"`.

- [ ] **Step 2: Run to verify failure**

Run: `ctest --test-dir build --output-on-failure -R tst_gui`
Expected: FAIL.

- [ ] **Step 3: Write `UrlName.h`**

```cpp
#pragma once
#include <QString>
#include <QUrl>
QString deriveFileName(const QUrl& url);
```

- [ ] **Step 4: Write `UrlName.cpp`**

```cpp
#include "UrlName.h"
#include <QFileInfo>

QString deriveFileName(const QUrl& url) {
    const QString path = url.path(QUrl::FullyDecoded);  // decodes %20 etc.
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("download") : name;
}
```

- [ ] **Step 5: Run to verify pass** — `ctest ... -R tst_gui` → PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/UrlName.h src/gui/UrlName.cpp tests/tst_gui.cpp
git commit -m "feat(gui): derive destination filename from URL"
```

---

### Task 5: GridGeometry (pure) — cells mapping

**Files:**
- Create: `src/gui/GridGeometry.h`, `src/gui/GridGeometry.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `Segment`, `DownloadState` (from `DownloadTypes.h`).
- Produces: `CellKind`, `Cell`, `computeCells(...)` (locked block).

- [ ] **Step 1: Write the failing tests**

```cpp
static QVector<Segment> segs2(qint64 total) {
    // two contiguous halves, each fully pending (current==start)
    Segment a{0, 0,          0, total/2 - 1};
    Segment b{1, total/2, total/2, total - 1};
    return {a, b};
}

void grid_all_pending_when_nothing_downloaded() {
    auto cells = computeCells(1000, segs2(1000), DownloadState::Downloading, 10);
    QCOMPARE(cells.size(), 10);
    for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Pending);
}
void grid_marks_downloaded_ranges_with_owner_segment() {
    auto s = segs2(1000);
    s[0].current = 500;                     // first half fully done -> cells 0..4 downloaded, owner 0
    auto cells = computeCells(1000, s, DownloadState::Downloading, 10);
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(cells[i].kind, CellKind::Downloaded);
        QCOMPARE(cells[i].segmentIndex, 0);
    }
    for (int i = 5; i < 10; ++i) QCOMPARE(cells[i].kind, CellKind::Pending);
}
void grid_full_download_all_downloaded() {
    auto s = segs2(1000); s[0].current = 500; s[1].current = 1000;
    auto cells = computeCells(1000, s, DownloadState::Completed, 8);
    for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
}
void grid_error_marks_incomplete_cells_error() {
    auto s = segs2(1000); s[0].current = 500;   // 2nd half pending
    auto cells = computeCells(1000, s, DownloadState::Error, 10);
    for (int i = 0; i < 5; ++i) QCOMPARE(cells[i].kind, CellKind::Downloaded);
    for (int i = 5; i < 10; ++i) QCOMPARE(cells[i].kind, CellKind::Error);
}
void grid_unknown_total_all_pending() {
    auto cells = computeCells(-1, {}, DownloadState::Downloading, 6);
    QCOMPARE(cells.size(), 6);
    for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Pending);
}
void grid_nonpositive_ncells_empty() {
    QCOMPARE(computeCells(1000, segs2(1000), DownloadState::Downloading, 0).size(), 0);
}
```

Add `#include "GridGeometry.h"`.

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `GridGeometry.h`**

```cpp
#pragma once
#include "DownloadTypes.h"
#include <QVector>

enum class CellKind { Pending, Downloaded, Error };
struct Cell {
    CellKind kind         = CellKind::Pending;
    int      segmentIndex = -1;
};
QVector<Cell> computeCells(qint64 totalBytes,
                           const QVector<Segment>& segments,
                           DownloadState state,
                           int nCells);
```

- [ ] **Step 4: Write `GridGeometry.cpp`**

```cpp
#include "GridGeometry.h"

static int ownerSegment(qint64 byte, const QVector<Segment>& segs) {
    for (const Segment& s : segs)
        if (byte >= s.start && (s.end < 0 || byte <= s.end)) return s.index;
    return -1;
}

QVector<Cell> computeCells(qint64 totalBytes,
                           const QVector<Segment>& segments,
                           DownloadState state,
                           int nCells) {
    QVector<Cell> cells;
    if (nCells <= 0) return cells;
    cells.resize(nCells);
    if (totalBytes <= 0) return cells;   // all Pending (indeterminate)

    for (int i = 0; i < nCells; ++i) {
        const qint64 cellStart = static_cast<qint64>(i)     * totalBytes / nCells;
        const qint64 cellEnd   = static_cast<qint64>(i + 1) * totalBytes / nCells; // exclusive
        const int owner = ownerSegment(cellStart, segments);
        bool downloaded = false;
        if (owner >= 0) {
            for (const Segment& s : segments)
                if (s.index == owner) { downloaded = (s.current >= cellEnd); break; }
        }
        if (downloaded) {
            cells[i].kind = CellKind::Downloaded;
            cells[i].segmentIndex = owner;
        } else if (state == DownloadState::Error) {
            cells[i].kind = CellKind::Error;
        } else {
            cells[i].kind = CellKind::Pending;
        }
    }
    return cells;
}
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/GridGeometry.h src/gui/GridGeometry.cpp tests/tst_gui.cpp
git commit -m "feat(gui): map file byte ranges to colored progress-grid cells"
```

---

### Task 6: SpeedSampler (pure) — speed + ETA

**Files:**
- Create: `src/gui/SpeedSampler.h`, `src/gui/SpeedSampler.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Produces: `class SpeedSampler` (locked block).

- [ ] **Step 1: Write the failing tests**

```cpp
void speed_zero_with_single_sample() {
    SpeedSampler s; s.addSample(0, 0);
    QCOMPARE(s.bytesPerSec(), 0.0);
    QCOMPARE(s.etaSeconds(1000), qint64(-1));
}
void speed_computes_rate_over_window() {
    SpeedSampler s;
    s.addSample(0,    0);
    s.addSample(1000, 1000);          // 1000 bytes in 1000 ms = 1000 B/s
    QVERIFY(qAbs(s.bytesPerSec() - 1000.0) < 1.0);
    QCOMPARE(s.etaSeconds(3000), qint64(2));  // 2000 bytes left / 1000 B/s
}
void speed_eta_unknown_when_total_unknown() {
    SpeedSampler s; s.addSample(0,0); s.addSample(500,1000);
    QCOMPARE(s.etaSeconds(-1), qint64(-1));
}
void speed_reset_clears() {
    SpeedSampler s; s.addSample(0,0); s.addSample(1000,1000);
    s.reset();
    QCOMPARE(s.bytesPerSec(), 0.0);
}
```

Add `#include "SpeedSampler.h"`.

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `SpeedSampler.h`**

```cpp
#pragma once
#include <QVector>
#include <QtGlobal>

class SpeedSampler {
public:
    void   addSample(qint64 bytes, qint64 tMs);
    double bytesPerSec() const;
    qint64 etaSeconds(qint64 totalBytes) const;
    void   reset();
private:
    struct Sample { qint64 bytes; qint64 tMs; };
    QVector<Sample> m_samples;
    static constexpr qint64 kWindowMs = 5000;
};
```

- [ ] **Step 4: Write `SpeedSampler.cpp`**

```cpp
#include "SpeedSampler.h"

void SpeedSampler::addSample(qint64 bytes, qint64 tMs) {
    m_samples.push_back({bytes, tMs});
    while (m_samples.size() > 2 && (tMs - m_samples.first().tMs) > kWindowMs)
        m_samples.removeFirst();
}

double SpeedSampler::bytesPerSec() const {
    if (m_samples.size() < 2) return 0.0;
    const Sample& a = m_samples.first();
    const Sample& b = m_samples.last();
    const qint64 dt = b.tMs - a.tMs;
    if (dt <= 0) return 0.0;
    const qint64 db = b.bytes - a.bytes;
    if (db <= 0) return 0.0;
    return (double(db) * 1000.0) / double(dt);
}

qint64 SpeedSampler::etaSeconds(qint64 totalBytes) const {
    if (totalBytes <= 0) return -1;
    const double bps = bytesPerSec();
    if (bps <= 0.0) return -1;
    const qint64 remaining = totalBytes - m_samples.last().bytes;
    if (remaining <= 0) return 0;
    return qint64(remaining / bps);
}

void SpeedSampler::reset() { m_samples.clear(); }
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/SpeedSampler.h src/gui/SpeedSampler.cpp tests/tst_gui.cpp
git commit -m "feat(gui): compute download speed and ETA from sampled progress"
```

---

### Task 7: DownloadTableModel

**Files:**
- Create: `src/gui/DownloadTableModel.h`, `src/gui/DownloadTableModel.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadManager` (Task 2), `FileType` (Task 3), `SpeedSampler` (Task 6),
  `DownloadTask::record()/id()/state()`, signals `DownloadManager::taskProgress/taskStateChanged`.
- Produces: `DownloadTableModel` (locked block).

- [ ] **Step 1a: Add local test helpers to `tst_gui.cpp`** (needed from this task on)

Put these file-scope helpers near the top of `tst_gui.cpp` (after the includes, before the test class). They mirror the ones in `tst_download.cpp` but are local to this translation unit:

```cpp
#include <QTemporaryDir>
#include <QFile>
#include <QDeadlineTimer>

static QString makeTempDir() {
    static QVector<QSharedPointer<QTemporaryDir>> keep;   // keep dirs alive for the test run
    auto d = QSharedPointer<QTemporaryDir>::create();
    keep.push_back(d);
    return d->path();
}
static QByteArray readFile(const QString& path) {
    QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
}
static bool waitForState(DownloadManager& mgr, const QUuid& id, DownloadState want, int timeoutMs) {
    QDeadlineTimer dl(timeoutMs);
    while (!dl.hasExpired()) {
        DownloadTask* t = mgr.taskById(id);
        if (t && t->state() == want) return true;
        QTest::qWait(20);
    }
    DownloadTask* t = mgr.taskById(id);
    return t && t->state() == want;
}
```

Add the includes `#include "DownloadManager.h"`, `#include "DownloadTask.h"`, `#include "DownloadTableModel.h"`, `#include "TestServer.h"` at the top of `tst_gui.cpp`. Also add `TestServer.cpp` to the `tst_gui` sources in `tests/CMakeLists.txt` now (the model tests drive a real manager against it):

```cmake
add_executable(tst_gui tst_gui.cpp TestServer.cpp)
```

- [ ] **Step 1: Write the failing tests**

Drive a real `DownloadManager` against `TestServer`. Verify rows and `dataChanged`.

```cpp
void model_rows_reflect_manager_tasks() {
    TestServer srv; srv.start();
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.columnCount(), int(DownloadTableModel::ColumnCount));

    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
    model.appendTask(mgr.taskById(id));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, DownloadTableModel::Name)).toString(), QString("out.bin"));
}
void model_emits_datachanged_on_progress() {
    TestServer srv; srv.start();
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
    model.appendTask(mgr.taskById(id));
    QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
    QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
    QVERIFY(spy.count() > 0);
}
void model_remove_drops_row() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    TestServer srv; srv.start();
    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/o.bin");
    model.appendTask(mgr.taskById(id));
    QCOMPARE(model.rowCount(), 1);
    QSignalSpy rem(&model, &QAbstractItemModel::rowsRemoved);
    model.removeTaskById(id);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(rem.count(), 1);
}
void model_exposes_state_and_category_roles() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    TestServer srv; srv.start();
    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.mp4");
    model.appendTask(mgr.taskById(id));
    QModelIndex ix = model.index(0, 0);
    QCOMPARE(model.data(ix, DownloadTableModel::CategoryRole).toInt(),
             int(FileType::Category::Movie));
    QVERIFY(model.data(ix, DownloadTableModel::StateRole).isValid());
}
```

- [ ] **Step 2: Run to verify failure** — FAIL (type incomplete / undefined).

- [ ] **Step 3: Write `DownloadTableModel.h`** (per locked block, plus private members)

```cpp
#pragma once
#include "DownloadTypes.h"
#include "SpeedSampler.h"
#include <QAbstractTableModel>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <QVector>
class DownloadManager;
class DownloadTask;

class DownloadTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Progress, Status, Speed, TimeLeft, ColumnCount };
    enum Roles  { StateRole = Qt::UserRole + 1, CategoryRole, ProgressRole, TaskRole };
    explicit DownloadTableModel(DownloadManager* mgr, QObject* parent = nullptr);
    int      rowCount(const QModelIndex& = {}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int, Qt::Orientation, int) const override;
    void          appendTask(DownloadTask* t);
    void          removeTaskById(const QUuid& id);
    DownloadTask* taskAt(int row) const;
private slots:
    void onTaskProgress(const QUuid& id, qint64 received, qint64 total);
    void onTaskStateChanged(const QUuid& id, DownloadState s);
    void onSpeedTick();
private:
    struct Row { DownloadTask* task; qint64 received = 0; qint64 total = -1; SpeedSampler sampler; };
    int rowForId(const QUuid& id) const;
    DownloadManager*  m_mgr;
    QVector<Row>      m_rows;
    QHash<QUuid,int>  m_index;      // id -> row
    QTimer            m_tick;
    QElapsedTimer     m_clock;
};
```

Add `#include <QElapsedTimer>`.

- [ ] **Step 4: Write `DownloadTableModel.cpp`**

Key points: connect to manager signals in the ctor; seed rows from `m_mgr->tasks()`; feed the sampler on progress; a 1 Hz timer updates Speed/TimeLeft.

```cpp
#include "DownloadTableModel.h"
#include "DownloadManager.h"
#include "DownloadTask.h"
#include "FileType.h"
#include <QFileInfo>
#include <QLocale>

DownloadTableModel::DownloadTableModel(DownloadManager* mgr, QObject* parent)
    : QAbstractTableModel(parent), m_mgr(mgr) {
    for (DownloadTask* t : m_mgr->tasks()) appendTask(t);
    connect(m_mgr, &DownloadManager::taskProgress,     this, &DownloadTableModel::onTaskProgress);
    connect(m_mgr, &DownloadManager::taskStateChanged, this, &DownloadTableModel::onTaskStateChanged);
    m_clock.start();
    connect(&m_tick, &QTimer::timeout, this, &DownloadTableModel::onSpeedTick);
    m_tick.start(1000);
}

int DownloadTableModel::rowCount(const QModelIndex&) const  { return m_rows.size(); }
int DownloadTableModel::columnCount(const QModelIndex&) const { return ColumnCount; }
DownloadTask* DownloadTableModel::taskAt(int row) const { return m_rows[row].task; }
int DownloadTableModel::rowForId(const QUuid& id) const { return m_index.value(id, -1); }

void DownloadTableModel::appendTask(DownloadTask* t) {
    const int row = m_rows.size();
    beginInsertRows({}, row, row);
    Row r; r.task = t; r.total = t->record().totalBytes;
    m_rows.push_back(r);
    m_index.insert(t->id(), row);
    endInsertRows();
}

void DownloadTableModel::removeTaskById(const QUuid& id) {
    const int row = rowForId(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    m_rows.remove(row);
    m_index.remove(id);
    for (int i = row; i < m_rows.size(); ++i) m_index[m_rows[i].task->id()] = i;
    endRemoveRows();
}

void DownloadTableModel::onTaskProgress(const QUuid& id, qint64 received, qint64 total) {
    const int row = rowForId(id);
    if (row < 0) return;
    m_rows[row].received = received;
    if (total > 0) m_rows[row].total = total;
    emit dataChanged(index(row, Progress), index(row, Progress), {Qt::DisplayRole, ProgressRole});
}

void DownloadTableModel::onTaskStateChanged(const QUuid& id, DownloadState s) {
    const int row = rowForId(id);
    if (row < 0) return;
    if (s == DownloadState::Completed || s == DownloadState::Error) m_rows[row].sampler.reset();
    emit dataChanged(index(row, Status), index(row, Status), {Qt::DisplayRole, StateRole});
}

void DownloadTableModel::onSpeedTick() {
    const qint64 now = m_clock.elapsed();
    for (int row = 0; row < m_rows.size(); ++row) {
        Row& r = m_rows[row];
        if (r.task->state() != DownloadState::Downloading) continue;
        r.sampler.addSample(r.received, now);
        emit dataChanged(index(row, Speed), index(row, TimeLeft), {Qt::DisplayRole});
    }
}

static QString stateText(DownloadState s) {
    switch (s) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
    }
    return {};
}

QVariant DownloadTableModel::data(const QModelIndex& ix, int role) const {
    if (!ix.isValid() || ix.row() >= m_rows.size()) return {};
    const Row& r = m_rows[ix.row()];
    const auto rec = r.task->record();
    const QString name = QFileInfo(rec.destPath).fileName();

    if (role == TaskRole)     return QVariant::fromValue(static_cast<void*>(r.task));
    if (role == StateRole)    return int(r.task->state());
    if (role == CategoryRole) return int(FileType::categorize(name));
    if (role == ProgressRole)
        return r.total > 0 ? int(100 * r.received / r.total) : 0;

    if (role != Qt::DisplayRole) return {};
    switch (ix.column()) {
        case Name:   return name;
        case Size:   return r.total > 0 ? QLocale().formattedDataSize(r.total) : QString("—");
        case Progress: return r.total > 0 ? QString::number(100 * r.received / r.total) + "%"
                                          : QString("—");
        case Status: return stateText(r.task->state());
        case Speed: {
            if (r.task->state() != DownloadState::Downloading) return QString();
            const double bps = r.sampler.bytesPerSec();
            return bps > 0 ? QLocale().formattedDataSize(qint64(bps)) + "/s" : QString();
        }
        case TimeLeft: {
            const qint64 eta = r.sampler.etaSeconds(r.total);
            if (eta < 0) return QString("—");
            return QString("%1:%2").arg(eta/60, 2, 10, QChar('0')).arg(eta%60, 2, 10, QChar('0'));
        }
    }
    return {};
}

QVariant DownloadTableModel::headerData(int s, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* h[] = {"Name","Size","Progress","Status","Speed","Time Left"};
    return (s >= 0 && s < ColumnCount) ? QString(h[s]) : QVariant();
}
```

- [ ] **Step 5: Run to verify pass** — `ctest ... -R tst_gui` → PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/DownloadTableModel.* tests/tst_gui.cpp tests/CMakeLists.txt
git commit -m "feat(gui): add DownloadTableModel driven by manager signals"
```

---

### Task 8: CategoryFilterProxy

**Files:**
- Create: `src/gui/CategoryFilterProxy.h`, `src/gui/CategoryFilterProxy.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadTableModel::StateRole/CategoryRole`, `FileType::Category`, `DownloadState`.
- Produces: `CategoryFilterProxy` + `Filter` enum (locked block).

- [ ] **Step 1: Write the failing tests**

```cpp
void proxy_filters_by_state_and_category() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    TestServer srv; srv.start();
    QUuid a = mgr.addDownload(srv.url("/ranged"), dir + "/movie.mp4");
    QUuid b = mgr.addDownload(srv.url("/ranged"), dir + "/song.mp3");
    model.appendTask(mgr.taskById(a));
    model.appendTask(mgr.taskById(b));

    CategoryFilterProxy proxy;
    proxy.setSourceModel(&model);

    proxy.setFilter(CategoryFilterProxy::Filter::All);
    QCOMPARE(proxy.rowCount(), 2);

    proxy.setFilter(CategoryFilterProxy::Filter::Movie);
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(proxy.data(proxy.index(0, DownloadTableModel::Name)).toString(), QString("movie.mp4"));

    proxy.setFilter(CategoryFilterProxy::Filter::Music);
    QCOMPARE(proxy.rowCount(), 1);
}
```

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `CategoryFilterProxy.h`**

```cpp
#pragma once
#include <QSortFilterProxyModel>
class CategoryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class Filter { All, Downloading, Completed, Movie, Software, Music, Others };
    void setFilter(Filter f);
protected:
    bool filterAcceptsRow(int srcRow, const QModelIndex& srcParent) const override;
private:
    Filter m_filter = Filter::All;
};
Q_DECLARE_METATYPE(CategoryFilterProxy::Filter)   // needed for QVariant::fromValue + QSignalSpy (Task 11)
```

- [ ] **Step 4: Write `CategoryFilterProxy.cpp`**

```cpp
#include "CategoryFilterProxy.h"
#include "DownloadTableModel.h"
#include "FileType.h"
#include "DownloadTypes.h"

void CategoryFilterProxy::setFilter(Filter f) { m_filter = f; invalidateFilter(); }

bool CategoryFilterProxy::filterAcceptsRow(int srcRow, const QModelIndex& parent) const {
    if (m_filter == Filter::All) return true;
    const QModelIndex ix = sourceModel()->index(srcRow, 0, parent);
    const auto state = DownloadState(ix.data(DownloadTableModel::StateRole).toInt());
    const auto cat   = FileType::Category(ix.data(DownloadTableModel::CategoryRole).toInt());
    switch (m_filter) {
        case Filter::Downloading: return state != DownloadState::Completed;
        case Filter::Completed:   return state == DownloadState::Completed;
        case Filter::Movie:       return cat == FileType::Category::Movie;
        case Filter::Software:    return cat == FileType::Category::Software;
        case Filter::Music:       return cat == FileType::Category::Music;
        case Filter::Others:      return cat == FileType::Category::Others;
        case Filter::All:         return true;
    }
    return true;
}
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/CategoryFilterProxy.* tests/tst_gui.cpp
git commit -m "feat(gui): filter downloads table by state and derived category"
```

---

### Task 9: ProgressGridWidget

**Files:**
- Create: `src/gui/ProgressGridWidget.h`, `src/gui/ProgressGridWidget.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `computeCells` (Task 5), `DownloadTask::segments()/state()/record()`, signal
  `DownloadTask::segmentProgress`.
- Produces: `ProgressGridWidget` (locked block).

- [ ] **Step 1: Write the failing smoke test** (widget logic is validated manually; here we check it constructs, accepts a task, and paints without crashing under offscreen)

```cpp
void grid_widget_constructs_and_sets_task() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    TestServer srv; srv.start();
    QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/o.bin");
    ProgressGridWidget w;
    w.resize(200, 80);
    w.setTask(mgr.taskById(id));
    w.setTask(nullptr);         // switching away must disconnect cleanly
    QVERIFY(true);
}
```

- [ ] **Step 2: Run to verify failure** — FAIL (incomplete type).

- [ ] **Step 3: Write `ProgressGridWidget.h`**

```cpp
#pragma once
#include <QWidget>
#include <QTimer>
class DownloadTask;

class ProgressGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProgressGridWidget(QWidget* parent = nullptr);
    void setTask(DownloadTask* t);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void scheduleRepaint();
    DownloadTask* m_task = nullptr;
    QTimer        m_repaint;
    static constexpr int kCellPx = 9;
};
```

- [ ] **Step 4: Write `ProgressGridWidget.cpp`**

```cpp
#include "ProgressGridWidget.h"
#include "GridGeometry.h"
#include "DownloadTask.h"
#include <QPainter>

static QColor segColor(int i) {
    static const QColor palette[] = {
        QColor("#3b82f6"), QColor("#10b981"), QColor("#f59e0b"), QColor("#8b5cf6"),
        QColor("#ec4899"), QColor("#14b8a6"), QColor("#f97316"), QColor("#6366f1"),
    };
    return palette[i % 8];
}

ProgressGridWidget::ProgressGridWidget(QWidget* parent) : QWidget(parent) {
    m_repaint.setSingleShot(true);
    connect(&m_repaint, &QTimer::timeout, this, [this]{ update(); });
}

void ProgressGridWidget::setTask(DownloadTask* t) {
    if (m_task) disconnect(m_task, nullptr, this, nullptr);
    m_task = t;
    if (m_task)
        connect(m_task, &DownloadTask::segmentProgress, this, [this]{ scheduleRepaint(); });
    update();
}

void ProgressGridWidget::scheduleRepaint() {
    if (!m_repaint.isActive()) m_repaint.start(100);   // throttle repaints
}

void ProgressGridWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#1e1e1e"));
    if (!m_task) return;
    const int cols = qMax(1, width()  / kCellPx);
    const int rows = qMax(1, height() / kCellPx);
    const int nCells = cols * rows;
    const auto rec  = m_task->record();
    const auto cells = computeCells(rec.totalBytes, m_task->segments(), m_task->state(), nCells);
    for (int i = 0; i < cells.size(); ++i) {
        const int cx = (i % cols) * kCellPx;
        const int cy = (i / cols) * kCellPx;
        QColor c;
        switch (cells[i].kind) {
            case CellKind::Downloaded: c = segColor(cells[i].segmentIndex); break;
            case CellKind::Error:      c = QColor("#ef4444"); break;
            case CellKind::Pending:    c = QColor("#3a3a3a"); break;
        }
        p.fillRect(cx, cy, kCellPx - 1, kCellPx - 1, c);
    }
}
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/ProgressGridWidget.* tests/tst_gui.cpp
git commit -m "feat(gui): paint the dense per-segment progress grid"
```

---

### Task 10: NewDownloadDialog

**Files:**
- Create: `src/gui/NewDownloadDialog.h`, `src/gui/NewDownloadDialog.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `deriveFileName` (Task 4).
- Produces: `NewDownloadDialog` (locked block).

- [ ] **Step 1: Write the failing tests** (validate the pure bits: `isValidHttpUrl` and `destPath` assembly; the visual dialog is validated manually)

```cpp
void dialog_validates_http_urls() {
    QVERIFY(NewDownloadDialog::isValidHttpUrl(QUrl("http://x.com/a")));
    QVERIFY(NewDownloadDialog::isValidHttpUrl(QUrl("https://x.com/a")));
    QVERIFY(!NewDownloadDialog::isValidHttpUrl(QUrl("ftp://x.com/a")));
    QVERIFY(!NewDownloadDialog::isValidHttpUrl(QUrl("")));
}
```

(`destPath()` composition is exercised through `deriveFileName`, already unit-tested in Task 4.)

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `NewDownloadDialog.h`**

```cpp
#pragma once
#include <QDialog>
#include <QUrl>
class QLineEdit;
class QLabel;

class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDownloadDialog(QWidget* parent = nullptr);
    QUrl        url() const;
    QString     destPath() const;
    static bool isValidHttpUrl(const QUrl& u);
private slots:
    void chooseDir();
    void refreshName();
private:
    QLineEdit* m_url;
    QLineEdit* m_dir;
    QLabel*    m_name;
};
```

- [ ] **Step 4: Write `NewDownloadDialog.cpp`**

```cpp
#include "NewDownloadDialog.h"
#include "UrlName.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>

bool NewDownloadDialog::isValidHttpUrl(const QUrl& u) {
    return !u.isEmpty() && (u.scheme() == "http" || u.scheme() == "https");
}

NewDownloadDialog::NewDownloadDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("New Download");
    m_url  = new QLineEdit(this);
    m_dir  = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), this);
    m_name = new QLabel("—", this);

    // Prefill from clipboard if it holds an http(s) URL.
    const QString clip = QApplication::clipboard()->text().trimmed();
    if (isValidHttpUrl(QUrl(clip))) m_url->setText(clip);

    auto* browse = new QPushButton("…", this);
    connect(browse, &QPushButton::clicked, this, &NewDownloadDialog::chooseDir);
    connect(m_url, &QLineEdit::textChanged, this, &NewDownloadDialog::refreshName);

    // "Save to" row: directory line-edit + browse button side by side.
    auto* dirRow = new QWidget(this);
    auto* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    dirLay->addWidget(m_dir);
    dirLay->addWidget(browse);

    auto* form = new QFormLayout(this);
    form->addRow("URL:", m_url);
    form->addRow("Save to:", dirRow);
    form->addRow("File:", m_name);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, [this]{
        if (isValidHttpUrl(url())) accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(box);
    refreshName();
}

void NewDownloadDialog::chooseDir() {
    const QString d = QFileDialog::getExistingDirectory(this, "Save to", m_dir->text());
    if (!d.isEmpty()) m_dir->setText(d);
}

void NewDownloadDialog::refreshName() {
    m_name->setText(deriveFileName(url()));
}

QUrl    NewDownloadDialog::url() const      { return QUrl(m_url->text().trimmed()); }
QString NewDownloadDialog::destPath() const { return m_dir->text() + "/" + deriveFileName(url()); }
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/NewDownloadDialog.* tests/tst_gui.cpp
git commit -m "feat(gui): add New Download dialog (URL + destination folder)"
```

---

### Task 11: CategoryTree

**Files:**
- Create: `src/gui/CategoryTree.h`, `src/gui/CategoryTree.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `CategoryFilterProxy::Filter` (Task 8).
- Produces: `CategoryTree` + `filterChanged(Filter)` (locked block).

- [ ] **Step 1: Write the failing test** (selecting a node emits the right filter)

```cpp
void tree_emits_filter_on_selection() {
    CategoryTree tree;
    QSignalSpy spy(&tree, &CategoryTree::filterChanged);
    // Select the "Completed" row (see build order in Step 3).
    tree.setCurrentItem(tree.topLevelItem(0)->child(1)); // All Downloads > Completed
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).value<CategoryFilterProxy::Filter>(),
             CategoryFilterProxy::Filter::Completed);
}
```

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `CategoryTree.h`**

```cpp
#pragma once
#include "CategoryFilterProxy.h"
#include <QTreeWidget>

class CategoryTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit CategoryTree(QWidget* parent = nullptr);
signals:
    void filterChanged(CategoryFilterProxy::Filter f);
};
```

- [ ] **Step 4: Write `CategoryTree.cpp`** (store the `Filter` in the item's data role; emit on selection)

```cpp
#include "CategoryTree.h"

using F = CategoryFilterProxy::Filter;

static QTreeWidgetItem* node(const QString& text, F f) {
    auto* it = new QTreeWidgetItem(QStringList{text});
    it->setData(0, Qt::UserRole, QVariant::fromValue(f));
    return it;
}

CategoryTree::CategoryTree(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    auto* root = node("All Downloads", F::All);
    root->addChild(node("Downloading", F::Downloading)); // index 0
    root->addChild(node("Completed",   F::Completed));   // index 1
    root->addChild(node("Movie",       F::Movie));       // index 2
    root->addChild(node("Software",    F::Software));     // index 3
    root->addChild(node("Music",       F::Music));        // index 4
    root->addChild(node("Others",      F::Others));       // index 5
    addTopLevelItem(root);
    expandAll();
    setCurrentItem(root);

    connect(this, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
            if (!cur) return;
            emit filterChanged(cur->data(0, Qt::UserRole).value<F>());
        });
}
```

- [ ] **Step 5: Run to verify pass** — PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/CategoryTree.* tests/tst_gui.cpp
git commit -m "feat(gui): add category tree emitting the active table filter"
```

---

### Task 12: MainWindow (assembly + wiring + Log/Properties)

**Files:**
- Create: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp` (replace stub)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: everything above — `DownloadManager` (pause/resume/remove/addDownload/taskById),
  `DownloadTableModel`, `CategoryFilterProxy`, `CategoryTree`, `ProgressGridWidget`,
  `NewDownloadDialog`, `FileType`.
- Produces: `MainWindow(DownloadManager*, DownloadTableModel*, QWidget*)`.

- [ ] **Step 1: Write the failing smoke test** (constructs, wires, destroys without crash)

```cpp
void mainwindow_constructs_and_wires() {
    EngineConfig cfg; QString dir = makeTempDir();
    DownloadManager mgr(cfg, dir);
    DownloadTableModel model(&mgr);
    MainWindow w(&mgr, &model);
    w.resize(900, 600);
    w.show();
    QVERIFY(w.findChild<QTableView*>() != nullptr);
    QVERIFY(w.findChild<CategoryTree*>() != nullptr);
    QVERIFY(w.findChild<ProgressGridWidget*>() != nullptr);
}
```

Add includes for `QTableView`, `MainWindow.h`.

- [ ] **Step 2: Run to verify failure** — FAIL.

- [ ] **Step 3: Write `MainWindow.h`**

```cpp
#pragma once
#include <QMainWindow>
class DownloadManager;
class DownloadTableModel;
class CategoryFilterProxy;
class ProgressGridWidget;
class QTableView;
class QPlainTextEdit;
class QFormLayout;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(DownloadManager* mgr, DownloadTableModel* model, QWidget* parent = nullptr);
private slots:
    void onNew();
    void onStart();
    void onPause();
    void onDelete();
    void onSelectionChanged();
    void onStateChanged(const QUuid& id, int state);
private:
    QUuid selectedId() const;
    DownloadManager*     m_mgr;
    DownloadTableModel*  m_model;
    CategoryFilterProxy* m_proxy;
    QTableView*          m_table;
    ProgressGridWidget*  m_grid;
    QPlainTextEdit*      m_log;
    QLabel*              m_props;
};
```

- [ ] **Step 4: Write `MainWindow.cpp`**

Assemble: toolbar (New/Start/Pause/Delete/Pause All/Resume All), left `CategoryTree`, central
`QTableView` on the proxy, bottom `QTabWidget` (Log / Progress / Properties). Wire selection → grid +
properties; tree → proxy filter; manager `taskStateChanged` → Log line.

```cpp
#include "MainWindow.h"
#include "DownloadManager.h"
#include "DownloadTask.h"
#include "DownloadTableModel.h"
#include "CategoryFilterProxy.h"
#include "CategoryTree.h"
#include "ProgressGridWidget.h"
#include "NewDownloadDialog.h"
#include <QAction>
#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTime>
#include <QToolBar>

MainWindow::MainWindow(DownloadManager* mgr, DownloadTableModel* model, QWidget* parent)
    : QMainWindow(parent), m_mgr(mgr), m_model(model) {
    // Toolbar
    auto* tb = addToolBar("Main");
    auto* aNew    = tb->addAction("New");
    auto* aStart  = tb->addAction("Start");
    auto* aPause  = tb->addAction("Pause");
    auto* aDelete = tb->addAction("Delete");
    tb->addSeparator();
    auto* aPauseAll  = tb->addAction("Pause All");
    auto* aResumeAll = tb->addAction("Resume All");
    connect(aNew,       &QAction::triggered, this, &MainWindow::onNew);
    connect(aStart,     &QAction::triggered, this, &MainWindow::onStart);
    connect(aPause,     &QAction::triggered, this, &MainWindow::onPause);
    connect(aDelete,    &QAction::triggered, this, &MainWindow::onDelete);
    connect(aPauseAll,  &QAction::triggered, this, [this]{ m_mgr->pauseAll(); });
    connect(aResumeAll, &QAction::triggered, this, [this]{ m_mgr->resumeAll(); });

    // Left tree + proxy + table
    m_proxy = new CategoryFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    auto* tree = new CategoryTree(this);
    connect(tree, &CategoryTree::filterChanged, this,
            [this](CategoryFilterProxy::Filter f){ m_proxy->setFilter(f); });

    m_table = new QTableView(this);
    m_table->setModel(m_proxy);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &MainWindow::onSelectionChanged);

    // Bottom tabs
    m_grid  = new ProgressGridWidget(this);
    m_log   = new QPlainTextEdit(this); m_log->setReadOnly(true);
    m_props = new QLabel("—", this);    m_props->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(m_log,   "Log");
    tabs->addTab(m_grid,  "Progress");
    tabs->addTab(m_props, "Properties");

    // Layout: [tree | table] over tabs
    auto* hsplit = new QSplitter(Qt::Horizontal, this);
    hsplit->addWidget(tree);
    hsplit->addWidget(m_table);
    hsplit->setStretchFactor(1, 1);
    auto* vsplit = new QSplitter(Qt::Vertical, this);
    vsplit->addWidget(hsplit);
    vsplit->addWidget(tabs);
    vsplit->setStretchFactor(0, 1);
    setCentralWidget(vsplit);

    connect(m_mgr, &DownloadManager::taskStateChanged, this,
        [this](const QUuid& id, DownloadState s){ onStateChanged(id, int(s)); });
}

QUuid MainWindow::selectedId() const {
    const QModelIndex cur = m_table->currentIndex();
    if (!cur.isValid()) return {};
    const QModelIndex src = m_proxy->mapToSource(cur);
    DownloadTask* t = m_model->taskAt(src.row());
    return t ? t->id() : QUuid();
}

void MainWindow::onNew() {
    NewDownloadDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QUuid id = m_mgr->addDownload(dlg.url(), dlg.destPath());
    m_model->appendTask(m_mgr->taskById(id));
}

void MainWindow::onStart()  { const QUuid id = selectedId(); if (!id.isNull()) m_mgr->resume(id); }
void MainWindow::onPause()  { const QUuid id = selectedId(); if (!id.isNull()) m_mgr->pause(id); }

void MainWindow::onDelete() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    const auto btn = QMessageBox::question(this, "Delete",
        "Remove this download? (partial file is kept)");
    if (btn != QMessageBox::Yes) return;
    m_mgr->remove(id, /*deleteFiles=*/false);
    m_model->removeTaskById(id);
}

void MainWindow::onSelectionChanged() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    m_grid->setTask(t);
    if (!t) { m_props->setText("—"); return; }
    const auto r = t->record();
    m_props->setText(QString("URL: %1\nDest: %2\nSize: %3\nSegments: %4\nRange: %5")
        .arg(r.url.toString(), r.destPath)
        .arg(r.totalBytes).arg(r.segmentCount).arg(r.supportsRange ? "yes" : "no"));
}

void MainWindow::onStateChanged(const QUuid& id, int state) {
    DownloadTask* t = m_mgr->taskById(id);
    const QString name = t ? QFileInfo(t->record().destPath).fileName() : id.toString();
    static const char* names[] = {"Queued","Connecting","Downloading","Paused","Completed","Error"};
    m_log->appendPlainText(QString("[%1] %2 → %3")
        .arg(QTime::currentTime().toString("hh:mm:ss"), name, names[state]));
}
```

- [ ] **Step 5: Run to verify pass** — `ctest ... -R tst_gui` → PASS.

- [ ] **Step 6: Commit** (ask first)

```bash
git add src/gui/MainWindow.* tests/tst_gui.cpp
git commit -m "feat(gui): assemble MainWindow with toolbar, tree, table, and tabs"
```

---

### Task 13: main_gui wiring + full-suite + manual E2E validation

**Files:**
- Modify: `src/gui/main_gui.cpp` (replace the Task 1 placeholder)
- Verify: full `ctest` suite

**Interfaces:**
- Consumes: `DownloadManager`, `DownloadTableModel`, `MainWindow`.

- [ ] **Step 1: Write the real `main_gui.cpp`** (note the init order: `loadSession()` before the model — see spec §3.3)

```cpp
#include <QApplication>
#include <QStandardPaths>
#include "DownloadManager.h"
#include "DownloadTableModel.h"
#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("orbit-gui");

    EngineConfig cfg;
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-gui";

    DownloadManager mgr(cfg, dataDir);
    mgr.loadSession();                    // restore tasks BEFORE building the model
    DownloadTableModel model(&mgr);       // ctor seeds rows from mgr.tasks()

    MainWindow w(&mgr, &model);
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(960, 640);
    w.show();
    return app.exec();
}
```

- [ ] **Step 2: Build and run the full test suite**

Run:
```bash
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: all targets pass — `tst_segmentation`, `tst_persistence`, `tst_download` (with the new
pause/resume cases), `tst_smoke`, `tst_gui`.

- [ ] **Step 3: Manual E2E validation (criteria 8–10)**

Launch the app and confirm, recording the result in the branch report:
```bash
./build/src/gui/orbit-gui
```
1. **New** → paste a real HTTP(S) URL (e.g. a public file) → choose a folder → OK. The row appears,
   Status goes Downloading, Progress/Speed/Time Left update live; on finish it becomes Completed and
   leaves the `Downloading` filter.
2. Select the row → **Pause** → row goes Paused; **Start** → resumes and completes; the downloaded
   file opens/verifies intact.
3. Open **Progress** tab → the dense colored grid fills as segments advance. Open **Properties** →
   URL/Dest/Size/Segments/Range shown. **Log** accumulates timestamped lifecycle lines.
4. Click **Movie**/**Music**/etc. in the left tree → the table filters by category; **Completed** and
   **Downloading** filter by state; **All Downloads** shows everything.

- [ ] **Step 4: Commit** (ask first)

```bash
git add src/gui/main_gui.cpp
git commit -m "feat(gui): wire orbit-gui entry point to Core session and table model"
```

---

## Self-Review notes (author)

- **Spec coverage:** §3.2 → Task 2; §3.3 model → Task 7; §3.4 tree/filter → Tasks 8/11 + FileType Task 3;
  §3.5 grid → Tasks 5/9; §3.6 speed → Tasks 6/7; §3.7 tabs → Task 12; §3.8 toolbar/dialog → Tasks 10/12;
  §5 file structure → Task 1; §6 build → Task 1; §7 tests → every task's TDD + Task 13 manual. All
  success criteria §2(1–7) map to automated tests; §2(8–10) map to Task 13 manual steps.
- **Pure-unit isolation:** `FileType`/`UrlName`/`GridGeometry`/`SpeedSampler` live in `orbitgui_logic`
  (QtCore only); the model/widgets live in `orbitgui` (Widgets). Cap invariant enforced in Task 2 and
  never bypassed by the GUI (Tasks 10/12 route through `resume/pause`).
- **Type consistency:** role/enum/method names in the locked block are used verbatim in Tasks 7/8/9/11/12.

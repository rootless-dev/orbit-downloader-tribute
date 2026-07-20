# Theme Support and Progress Grid Fill Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Light/Dark/System theme (selectable in Preferences, persisted) and fix the download progress grid so a completed download fills every cell.

**Architecture:** Theming leans on Qt's own `QStyleHints::setColorScheme()` (Qt 6.8+) to restyle the whole app and follow the OS live in System mode — no hand-built palettes, no QSS. A small `Theme` module maps the persisted `ThemePref` to `Qt::ColorScheme` and exposes a `GridColors` palette (neutrals from the current `QPalette`, Orbit status colors fixed). The grid bug is a pure-logic fix in `computeCells`.

**Tech Stack:** C++/Qt6 (6.11.1), Qt Widgets, QtTest, CMake.

## Global Constraints

- Qt version floor: **6.8+** (we run 6.11.1) — required for `QStyleHints::setColorScheme()`.
- No QSS stylesheets; theming goes through the native palette via `setColorScheme`.
- Settings persist to `settings.json` via `SettingsIo` (never `QSettings`).
- Orbit status colors are fixed across both themes: downloaded `#5b9bd5`, active `#f7941e`, error `#ef4444`. Only neutrals follow the theme.
- Commit messages: English, Conventional Commits, no co-authorship trailer. **Do not commit without the user's explicit authorization** — the plan lists commit steps, but pause and ask before each commit until the user says otherwise.
- `Settings.cpp` lives in `orbitgui_logic` (headless-testable); `Theme.cpp`, `PreferencesDialog.cpp`, `ProgressGridWidget.cpp`, `MainWindow.cpp` live in `orbitgui` (Qt Widgets).

---

### Task 1: ThemePref enum + JSON serialization

**Files:**
- Modify: `src/gui/Settings.h`
- Modify: `src/gui/Settings.cpp`
- Test: `tests/tst_settings.cpp`

**Interfaces:**
- Produces: `enum class ThemePref { System, Light, Dark };` (in `Settings.h`), field `ThemePref theme = ThemePref::System;` on `UiPrefs`. JSON key `ui.theme` with values `"system" | "light" | "dark"`.

- [ ] **Step 1: Write the failing tests**

Add these two slots inside `class TstSettings` in `tests/tst_settings.cpp` (e.g. right after `appSettingsRoundTripThroughFile`):

```cpp
    void themeRoundTrips() {
        AppSettings s;
        s.ui.theme = ThemePref::Dark;
        const QJsonObject j = SettingsIo::toJson(s, QJsonObject{});
        QCOMPARE(j.value("ui").toObject().value("theme").toString(), QString("dark"));
        const AppSettings back = SettingsIo::fromJson(j, EngineConfig{});
        QVERIFY(back.ui.theme == ThemePref::Dark);
    }
    void themeDefaultsToSystemWhenMissing() {
        const AppSettings back = SettingsIo::fromJson(QJsonObject{}, EngineConfig{});
        QVERIFY(back.ui.theme == ThemePref::System);
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R tst_settings -V`
Expected: FAIL to compile (`ThemePref` / `UiPrefs::theme` undefined).

- [ ] **Step 3: Add the enum and field in `Settings.h`**

Add the enum just above `struct UiPrefs` (after `enum class Recurrence` at line 8 is fine too — keep it near `UiPrefs`):

```cpp
enum class ThemePref { System, Light, Dark };

struct UiPrefs {
    QString       defaultDownloadDir;                 // vazio => boot resolve p/ Downloads
    ClipboardMode clipboardMode = ClipboardMode::Off;
    ThemePref     theme         = ThemePref::System;
};
```

- [ ] **Step 4: Add serialization helpers and wire them in `Settings.cpp`**

Add helpers next to `clipToStr`/`clipFromStr` (after line 18):

```cpp
static QString themeToStr(ThemePref t) {
    switch (t) {
        case ThemePref::Light: return "light";
        case ThemePref::Dark:  return "dark";
        case ThemePref::System: default: return "system";
    }
}
static ThemePref themeFromStr(const QString& s) {
    if (s == "light") return ThemePref::Light;
    if (s == "dark")  return ThemePref::Dark;
    return ThemePref::System;
}
```

In `fromJson`, in the `ui` block (after line 32, `s.ui.clipboardMode = ...`):

```cpp
    s.ui.theme = themeFromStr(ui.value("theme").toString());
```

In `toJson`, extend the `ui` object (lines 53-55) to include the theme:

```cpp
    root["ui"]       = QJsonObject{
        {"defaultDownloadDir", s.ui.defaultDownloadDir},
        {"clipboardMode",      clipToStr(s.ui.clipboardMode)},
        {"theme",              themeToStr(s.ui.theme)}};
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R tst_settings -V`
Expected: PASS (all `TstSettings` slots, including the two new ones).

- [ ] **Step 6: Commit** (ask for authorization first)

```bash
git add src/gui/Settings.h src/gui/Settings.cpp tests/tst_settings.cpp
git commit -m "feat(settings): persist Light/Dark/System theme preference"
```

---

### Task 2: Fix progress grid fill on completed downloads

**Files:**
- Modify: `src/gui/GridGeometry.cpp:9-45` (`computeCells`)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `computeCells(qint64 totalBytes, const QVector<Segment>&, DownloadState, int nCells)` — signature unchanged.
- Segment semantics (from `DownloadTypes.h`): `Segment{index, start, current, end}`; a segment is complete when `current == end + 1` (`current > end`). Segments are contiguous and cover `[0, totalBytes)`.

- [ ] **Step 1: Write the failing tests**

Add these two slots to `tst_gui.cpp` next to the other `grid_*` tests (e.g. after `grid_full_download_all_downloaded`):

```cpp
    void grid_straddling_boundary_all_downloaded() {
        // Segment boundary at 500 misaligned with 7 cells (~142 bytes each):
        // cell 3 spans [428,571), crossing the boundary. Pre-fix it stayed
        // Pending (owner-of-start test could never reach cellEnd). Uses Queued
        // to exercise the range logic, NOT the Completed short-circuit.
        auto s = segs2(1000);
        s[0].current = 500;    // seg0 [0..499] complete
        s[1].current = 1000;   // seg1 [500..999] complete
        auto cells = computeCells(1000, s, DownloadState::Queued, 7);
        QCOMPARE(cells.size(), 7);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
    }
    void grid_completed_fills_all_even_misaligned() {
        // Completed short-circuit must fill every cell even when boundaries
        // don't align to cell edges.
        auto s = segs2(1000); s[0].current = 500; s[1].current = 1000;
        auto cells = computeCells(1000, s, DownloadState::Completed, 7);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: `grid_straddling_boundary_all_downloaded` FAILS (boundary cell reports `Pending`, not `Downloaded`). `grid_completed_fills_all_even_misaligned` may already pass (existing Completed branch is coarse) — that's fine; keep it as a regression guard.

- [ ] **Step 3: Rewrite `computeCells` in `GridGeometry.cpp`**

Replace the whole body of the `for` loop (lines 18-42) so "downloaded" considers **every** segment overlapping the cell, plus a `Completed` short-circuit. Full replacement of `computeCells` (keep the `ownerSegment` helper and the head of the function unchanged):

```cpp
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

        // Downloaded iff every segment overlapping [cellStart, cellEnd) has
        // fetched its whole portion of the cell. This covers cells that
        // straddle a segment boundary (the old code only checked the segment
        // owning cellStart, leaving boundary cells gray at 100%).
        bool anyCover = false;
        bool downloaded = true;
        for (const Segment& s : segments) {
            const qint64 sEndExcl = (s.end < 0) ? cellEnd : s.end + 1; // exclusive end
            const bool overlaps = s.start < cellEnd && sEndExcl > cellStart;
            if (!overlaps) continue;
            anyCover = true;
            if (s.current < qMin(cellEnd, sEndExcl)) { downloaded = false; break; }
        }
        downloaded = downloaded && anyCover;
        if (state == DownloadState::Completed) downloaded = true;  // defensive fill

        qint64 cur = -1;
        for (const Segment& s : segments)
            if (s.index == owner) { cur = s.current; break; }
        const bool active = (state == DownloadState::Downloading ||
                             state == DownloadState::Connecting);
        if (downloaded) {
            cells[i].kind = CellKind::Downloaded;
            cells[i].segmentIndex = owner;
        } else if (active && owner >= 0 && cellStart <= cur && cur < cellEnd) {
            cells[i].kind = CellKind::Active;         // célula que contém o current
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

Note: `#include <QtGlobal>` (for `qMin`) is already transitively available via the Qt containers included by `GridGeometry.h`; if the build complains, add `#include <QtGlobal>` at the top of `GridGeometry.cpp`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: PASS. Verify the pre-existing grid tests still pass too: `grid_all_pending_when_nothing_downloaded`, `grid_marks_downloaded_ranges_with_owner_segment`, `grid_full_download_all_downloaded`, `grid_error_marks_incomplete_cells_error`, `computeCellsMarksActiveHead`.

- [ ] **Step 5: Commit** (ask for authorization first)

```bash
git add src/gui/GridGeometry.cpp tests/tst_gui.cpp
git commit -m "fix(grid): fill cells straddling segment boundaries at 100%"
```

---

### Task 3: Theme module (applyTheme + gridColors)

**Files:**
- Create: `src/gui/Theme.h`
- Create: `src/gui/Theme.cpp`
- Modify: `src/gui/CMakeLists.txt:17-28` (add `Theme.cpp` to the `orbitgui` target)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `ThemePref` (from `Settings.h`, Task 1).
- Produces:
  - `void applyTheme(ThemePref pref);`
  - `struct GridColors { QColor background, downloaded, active, error, pending; };`
  - `GridColors gridColors();`

- [ ] **Step 1: Write the failing test**

Add to `tst_gui.cpp`. Include the header near the other GUI includes (top of file): `#include "Theme.h"`. Then add a slot:

```cpp
    void gridColors_keeps_orbit_identity_colors() {
        const GridColors c = gridColors();
        QCOMPARE(c.downloaded, QColor("#5b9bd5"));
        QCOMPARE(c.active,     QColor("#f7941e"));
        QCOMPARE(c.error,      QColor("#ef4444"));
        QVERIFY(c.background.isValid());
        QVERIFY(c.pending.isValid());
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: FAIL to compile (`Theme.h` / `gridColors` not found).

- [ ] **Step 3: Create `src/gui/Theme.h`**

```cpp
#pragma once
#include "Settings.h"   // ThemePref
#include <QColor>

// Cores do progress grid: neutros seguem a QPalette do tema atual; as cores de
// identidade Orbit (downloaded/active/error) são fixas nos dois temas.
struct GridColors {
    QColor background;
    QColor downloaded;
    QColor active;
    QColor error;
    QColor pending;
};

// Aplica o tema à app inteira via QStyleHints::setColorScheme (Qt 6.8+).
// System => Unknown (segue o SO ao vivo); Light/Dark => forçado.
void applyTheme(ThemePref pref);

// Paleta do grid derivada do tema/QPalette atuais.
GridColors gridColors();
```

- [ ] **Step 4: Create `src/gui/Theme.cpp`**

```cpp
#include "Theme.h"
#include <QApplication>
#include <QStyleHints>
#include <QPalette>

void applyTheme(ThemePref pref) {
    if (!qApp) return;
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;   // System: segue o SO ao vivo
    switch (pref) {
        case ThemePref::Light: scheme = Qt::ColorScheme::Light; break;
        case ThemePref::Dark:  scheme = Qt::ColorScheme::Dark;  break;
        case ThemePref::System: default: scheme = Qt::ColorScheme::Unknown; break;
    }
    qApp->styleHints()->setColorScheme(scheme);
}

GridColors gridColors() {
    const QPalette pal = qApp ? qApp->palette() : QPalette();
    GridColors c;
    c.background = pal.color(QPalette::Base);   // fundo da área (segue o tema)
    c.pending    = pal.color(QPalette::Mid);    // neutro que segue o tema
    c.downloaded = QColor("#5b9bd5");           // azul Orbit (fixo)
    c.active     = QColor("#f7941e");           // laranja (fixo)
    c.error      = QColor("#ef4444");           // vermelho (fixo)
    return c;
}
```

- [ ] **Step 5: Register `Theme.cpp` in the `orbitgui` target**

In `src/gui/CMakeLists.txt`, add `Theme.cpp` to the `orbitgui` source list (the block starting at line 17, `add_library(orbitgui STATIC`). Insert it after `ProgressGridWidget.cpp`:

```cmake
add_library(orbitgui STATIC
    DownloadTableModel.cpp
    CategoryFilterProxy.cpp
    ProgressGridWidget.cpp
    Theme.cpp
    NewDownloadDialog.cpp
    CategoryTree.cpp
    ClipboardWatcher.cpp
    CredentialsDialog.cpp
    PreferencesDialog.cpp
    SchedulerDialog.cpp
    MainWindow.cpp
)
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake -S . -B build && cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: PASS (re-run CMake configure so the new source is picked up).

- [ ] **Step 7: Commit** (ask for authorization first)

```bash
git add src/gui/Theme.h src/gui/Theme.cpp src/gui/CMakeLists.txt tests/tst_gui.cpp
git commit -m "feat(theme): add Theme module (applyTheme + grid palette)"
```

---

### Task 4: Appearance tab + apply theme at runtime

**Files:**
- Modify: `src/gui/PreferencesDialog.h`
- Modify: `src/gui/PreferencesDialog.cpp`
- Modify: `src/gui/MainWindow.cpp:539-551` (`applySettings`)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `ThemePref` (Task 1), `applyTheme` (Task 3), `PreferencesDialog::result()`.
- Produces: an "Appearance" tab (first tab) with a theme `QComboBox`; `result().ui.theme` reflects the selection; `MainWindow::applySettings` calls `applyTheme(s.ui.theme)`.

- [ ] **Step 1: Write the failing tests**

Add to `tst_gui.cpp`. This tests two things: the dialog round-trips the theme, and `applySettings` pushes it to the app's color scheme.

```cpp
    void preferences_roundtrips_theme() {
        AppSettings in;
        in.ui.theme = ThemePref::Dark;
        PreferencesDialog dlg(in);
        QVERIFY(dlg.result().ui.theme == ThemePref::Dark);
    }
    void applySettings_applies_color_scheme() {
        // MainWindow requires a manager/model/logger; reuse the harness's
        // existing helpers if present. If constructing MainWindow here is
        // heavy, this assertion can instead call applyTheme() directly.
        applyTheme(ThemePref::Dark);
        QCOMPARE(qApp->styleHints()->colorScheme(), Qt::ColorScheme::Dark);
        applyTheme(ThemePref::Light);
        QCOMPARE(qApp->styleHints()->colorScheme(), Qt::ColorScheme::Light);
        applyTheme(ThemePref::System);   // restore live-follow for other tests
    }
```

Add `#include <QStyleHints>` to the test file's includes if not already present.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: FAIL to compile (`m_theme` wiring absent; `result().ui.theme` compiles but returns default `System`, so `preferences_roundtrips_theme` FAILS at runtime).

- [ ] **Step 3: Declare the theme combo in `PreferencesDialog.h`**

Add the member (after the `m_base` line, before the `// General` group at line 23):

```cpp
    // Appearance
    QComboBox* m_theme      = nullptr;
```

- [ ] **Step 4: Build the Appearance tab as the first tab in `PreferencesDialog.cpp`**

In the constructor, immediately after `auto* tabs = new QTabWidget(this);` (line 26), insert:

```cpp
    // ---- Appearance (first tab) ----
    auto* appr = new QWidget(this);
    auto* apf  = new QFormLayout(appr);
    m_theme = new QComboBox(appr);
    m_theme->addItem(tr("System"), int(ThemePref::System));
    m_theme->addItem(tr("Light"),  int(ThemePref::Light));
    m_theme->addItem(tr("Dark"),   int(ThemePref::Dark));
    m_theme->setCurrentIndex(m_theme->findData(int(current.ui.theme)));
    apf->addRow(tr("Theme:"), m_theme);
    tabs->addTab(appr, tr("Appearance"));
```

Because this `addTab` runs before the General/Advanced/Browser `addTab` calls, Appearance becomes the first tab.

- [ ] **Step 5: Persist the selection in `result()`**

In `PreferencesDialog::result()`, next to the other `s.ui.*` assignments (after line 162, `s.ui.clipboardMode = ...`):

```cpp
    s.ui.theme = ThemePref(m_theme->currentData().toInt());
```

- [ ] **Step 6: Apply the theme in `MainWindow::applySettings`**

Add `#include "Theme.h"` to `MainWindow.cpp`'s includes. In `applySettings` (after line 541, `m_settingsPath = settingsPath;`):

```cpp
    applyTheme(s.ui.theme);
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: PASS (`preferences_roundtrips_theme`, `applySettings_applies_color_scheme`).

- [ ] **Step 8: Commit** (ask for authorization first)

```bash
git add src/gui/PreferencesDialog.h src/gui/PreferencesDialog.cpp src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(prefs): add Appearance tab and apply theme live"
```

---

### Task 5: Progress grid paints with theme colors

**Files:**
- Modify: `src/gui/ProgressGridWidget.cpp:72-92` (`paintEvent`)

**Interfaces:**
- Consumes: `gridColors()` (Task 3).

This is the visible payoff: the grid's fixed white background and hardcoded cell colors are replaced by `gridColors()`, so the grid follows the active theme (fixing the white area under the grid in dark mode) while keeping the Orbit status colors. No new unit test — verified by running the app (a paint-only change).

- [ ] **Step 1: Replace hardcoded colors in `paintEvent`**

Add `#include "Theme.h"` to `ProgressGridWidget.cpp`'s includes. Replace the body of `paintEvent` (lines 72-92) with:

```cpp
void ProgressGridWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const GridColors col = gridColors();
    p.fillRect(rect(), col.background);
    if (!m_task) return;
    const int cols   = qMax(1, width() / kCellPx);
    const int nCells = qMax(cellCount(), cols);
    const auto rec   = m_task->record();
    const auto cells = computeCells(rec.totalBytes, m_task->segments(), m_task->state(), nCells);
    for (int i = 0; i < cells.size(); ++i) {
        const int cx = (i % cols) * kCellPx;
        const int cy = (i / cols) * kCellPx;
        QColor c;
        switch (cells[i].kind) {
            case CellKind::Downloaded: c = col.downloaded; break;
            case CellKind::Active:     c = col.active;     break;
            case CellKind::Error:      c = col.error;      break;
            case CellKind::Pending:    c = col.pending;    break;
        }
        p.fillRect(cx, cy, kCellPx - 1, kCellPx - 1, c);
    }
}
```

- [ ] **Step 2: Build and run the app to verify**

Run: `cmake --build build --target orbit-gui`
Then launch the app (use the `verify` or `run` skill). Confirm, with a completed download selected on the Progress tab:
- No gray cells remain at 100% (Task 2 fix).
- The grid background follows the theme (dark background in Dark mode, not white).
- Switching Preferences → Appearance → Light/Dark/System restyles the whole window live, and the grid recolors with it.

- [ ] **Step 3: Full test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (timing-flaky tests may be skipped via `ORBIT_SKIP_TIMING_TESTS` per issue #1).

- [ ] **Step 4: Commit** (ask for authorization first)

```bash
git add src/gui/ProgressGridWidget.cpp
git commit -m "fix(grid): paint progress grid with theme-aware colors"
```

---

## Self-Review Notes

- **Spec coverage:** Theme enum+persistence (Task 1) ✓; grid straddle+Completed fix (Task 2) ✓; Theme module applyTheme+gridColors (Task 3) ✓; Appearance tab + live apply (Task 4) ✓; grid paints theme colors incl. background (Task 5) ✓; tests for straddle/Completed/theme round-trip (Tasks 1,2,4) ✓.
- **Simplification vs. spec:** the spec listed a separate `main_gui.cpp` edit; dropped because `main_gui.cpp` already calls `w.applySettings()` at boot, so `applyTheme` inside `applySettings` (Task 4) covers boot + runtime.
- **Type consistency:** `ThemePref`, `applyTheme(ThemePref)`, `GridColors{background,downloaded,active,error,pending}`, `gridColors()` used identically across Tasks 1/3/4/5. JSON key `ui.theme` consistent between `toJson`/`fromJson`.
- **Live System follow:** handled by Qt automatically when scheme is `Unknown` (no manual `colorSchemeChanged` wiring needed).

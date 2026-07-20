# Theme Support and Progress Grid Fill Fix — Design

Date: 2026-07-20
Status: Approved (pending spec review)

## Summary

Two related UI fixes for the Qt6 GUI:

1. **Theme support** — add a Light / Dark / System theme, selectable from Preferences and
   persisted in settings. The app currently has no theming at all (native default style,
   no palette, no system-theme detection).
2. **Progress grid fill bug** — some cells in the download progress grid stay gray even
   when the download reaches 100%. Fix the cell-color logic so a completed download fills
   every cell.

Both touch the same area (progress grid colors), so they ship as one change.

## Background / Current State

- **No theming exists.** `src/gui/main_gui.cpp` creates a plain `QApplication` with no
  `setStyle`, `QPalette`, or system color-scheme detection. There is zero QSS in the repo.
- The only explicit colors in the app are 5 hardcoded literals in
  `src/gui/ProgressGridWidget.cpp::paintEvent` (background `#ffffff`, downloaded `#5b9bd5`,
  active `#f7941e`, error `#ef4444`, pending `#dcdcdc`). The fixed white background is why
  the area under the grid renders white even under a dark OS theme.
- Settings persist to `settings.json` (via `SettingsIo`, **not** `QSettings`).
  `UiPrefs` (`src/gui/Settings.h`) currently holds only `defaultDownloadDir` and
  `clipboardMode` — no theme field.
- `PreferencesDialog` is a `QDialog` with a `QTabWidget` of 3 tabs (General / Advanced /
  Browser) and no appearance section. It does not persist anything itself; `result()`
  rebuilds an `AppSettings` from the widgets and the caller saves it.
- `MainWindow::applySettings()` is where confirmed settings take runtime effect.
- Qt version available: **6.11.1**, so `QStyleHints::setColorScheme()` (Qt 6.8+) and
  `colorSchemeChanged` (Qt 6.5+) are usable.

## The Progress Grid Bug

`src/gui/GridGeometry.cpp::computeCells` decides each cell's color from **only the segment
that owns the cell's start byte**:

```cpp
const int owner = ownerSegment(cellStart, segments);       // owner of START only
if (owner >= 0)
    downloaded = (s.current >= cellEnd);                    // that one segment
```

Download segments are contiguous (`Segmentation.cpp`: each `start = previous.end + 1`) and
the number of grid tiles derives from file size (256..16384), independent of segment
boundaries. So a cell can straddle the boundary between segment A and segment B. For such a
cell, `owner` is A, but a completed A saturates at `A.current = A.end + 1`, which is less
than `cellEnd`. The test `A.current >= cellEnd` is therefore false and the cell falls to
`Pending` (gray) — even when both A and B are 100% complete.

With the default of 4 segments this yields ~3 permanently-gray cells at the segment
boundaries, matching the reported screenshot. Existing tests miss it because
`tst_gui.cpp` uses `nCells` values that align cell edges to the segment boundary.

## Design

### Part A — Theme system

Core approach: let Qt apply the native light/dark palette instead of hand-building
`QPalette`s. `QStyleHints::setColorScheme(Qt::ColorScheme)` restyles the whole app
(windows, lists, dialogs, toolbar) and, when set to `Qt::ColorScheme::Unknown`, follows the
OS live. Mapping:

| ThemePref | Qt::ColorScheme  | Behavior                          |
|-----------|------------------|-----------------------------------|
| System    | Unknown          | Follows the OS live (auto switch) |
| Light     | Light            | Forced light                      |
| Dark      | Dark             | Forced dark                       |

Changes:

1. **`src/gui/Settings.h`**
   - Add `enum class ThemePref { System, Light, Dark };`
   - Add `ThemePref theme = ThemePref::System;` to `UiPrefs`.

2. **`src/gui/Settings.cpp`**
   - Serialize `ui.theme` inside the existing `ui` JSON block in `fromJson`/`toJson`
     as a string: `"system" | "light" | "dark"`. Unknown/missing value falls back to
     `System`.

3. **New `src/gui/Theme.h` / `src/gui/Theme.cpp`**
   - `void applyTheme(ThemePref pref);` — maps `ThemePref` to `Qt::ColorScheme` and calls
     `qApp->styleHints()->setColorScheme(...)`.
   - `struct GridColors { QColor background, downloaded, active, error, pending; };`
   - `GridColors gridColors();` — reads neutral colors (background, pending) from the
     current `QPalette` so they follow the active theme, while keeping the Orbit identity
     colors fixed across both themes: downloaded `#5b9bd5`, active `#f7941e`, error
     `#ef4444`. (No separate grid-line color: the widget draws each cell at `kCellPx - 1`,
     so the 1px gap already reveals the background as the grid line.)
   - Register `Theme.cpp` in the `orbitgui` widgets target (`src/gui/CMakeLists.txt`),
     which links Qt Widgets — not `orbitgui_logic`, which is pure/testable.

4. **`src/gui/PreferencesDialog.h` / `.cpp`**
   - Add an **"Appearance"** tab as the **first** tab, containing a single labeled
     `QComboBox` with items System / Light / Dark.
   - Initialize the combo from the incoming `AppSettings.ui.theme`.
   - Write the selected value back in `result()`.

5. **`src/gui/MainWindow.cpp` `applySettings()`**
   - Call `applyTheme(settings.ui.theme)` so confirming Preferences applies the theme live.

6. **`src/gui/main_gui.cpp`**
   - Call `applyTheme(settings.ui.theme)` at boot, after constructing `QApplication` and
     loading settings, before showing the window.

### Part B — Progress grid fill fix

1. **`src/gui/ProgressGridWidget.cpp::paintEvent`**
   - Replace the 5 hardcoded color literals (including the fixed `#ffffff` background) with
     values from `gridColors()`, so the grid follows the active theme.

2. **`src/gui/GridGeometry.cpp::computeCells`** — change the "downloaded" decision to
   consider **every segment that overlaps `[cellStart, cellEnd)`**, not just the owner of
   the start byte:

   ```
   A cell is Downloaded iff:
     - at least one segment overlaps [cellStart, cellEnd), AND
     - for every segment s overlapping the cell: s.current >= min(cellEnd, s.end + 1)
   ```

   This marks straddling cells correctly once all covering segments are complete.
   Additionally, short-circuit: if `state == DownloadState::Completed`, mark every cell
   `Downloaded` (guards against any rounding in `current`).

   The `Active` (contains `current` while Downloading/Connecting) and `Error` branches
   are preserved.

## Testing (TDD)

Add to `tests/tst_gui.cpp`:

1. **Straddling boundary** — a case where the segment boundary is misaligned with cell
   edges, e.g. `segs2(1000)` (boundary at 500) with `nCells = 7`. Before the fix the
   boundary cell is `Pending`; after the fix all cells are `Downloaded`. This locks the
   regression.
2. **Completed short-circuit** — `state == Completed` yields all `Downloaded` cells.
3. **Theme round-trip** — `SettingsIo::fromJson`/`toJson` round-trips `ThemePref`: default
   is `System`, and each of System/Light/Dark survives a save+load cycle. Missing/invalid
   JSON value falls back to `System`.

`applyTheme` itself (a thin wrapper over `QStyleHints`) is verified by manual inspection in
the running app, not a unit test.

## Out of Scope

- Accent-color customization, UI density, or any per-theme tuning of the Orbit status
  colors (downloaded/active/error stay fixed across themes).
- Custom QSS stylesheets — theming relies on Qt's native palette via `setColorScheme`.
- Migrating settings storage away from `settings.json`.

## Files Touched

- `src/gui/Settings.h`, `src/gui/Settings.cpp`
- `src/gui/Theme.h` (new), `src/gui/Theme.cpp` (new)
- `src/gui/PreferencesDialog.h`, `src/gui/PreferencesDialog.cpp`
- `src/gui/MainWindow.cpp`
- `src/gui/main_gui.cpp`
- `src/gui/GridGeometry.cpp`
- `src/gui/ProgressGridWidget.cpp`
- `src/gui/CMakeLists.txt` (register `Theme.cpp` in the `orbitgui` target)
- `tests/tst_gui.cpp`

# Preferences: sidebar-of-categories redesign (classic Orbit style)

Date: 2026-07-22
Status: design approved, pending implementation plan

## Goal

Refactor the Preferences screen (`PreferencesDialog`) to the classic Orbit
Downloader layout: a **vertical category list on the left** and a **content
panel on the right** with a section header, replacing the current horizontal
`QTabWidget`. The Scheduler, today a separate dialog, becomes a category inside
Preferences.

The refactor is **local to `PreferencesDialog`** (plus removing
`SchedulerDialog` and small `MainWindow` adjustments). It does not touch the
persistence layer (`Settings.*` / `SettingsIo`) nor the JSON format.

## Scope

Included:
- Replace `QTabWidget` with `QListWidget` (categories) + `QStackedWidget`
  (pages), native style honoring the `QPalette` (Light/Dark/System).
- Reorganize the current settings into categories with Orbit-style names.
- Section header at the top of each page.
- Footer with **Restore defaults** (left) + **OK/Cancel** (right).
- Bring the Scheduler options into a "Scheduler" category; remove the standalone
  `SchedulerDialog` and redirect its action to open Preferences already on that
  category.
- Placeholder "coming soon" categories for unimplemented features.

Out of scope:
- Any change to `Settings.h/.cpp`, `SettingsIo`, JSON format, or defaults.
- Introducing QSS/stylesheet (explicit decision: native style via QPalette).
- Implementing the placeholder categories' features (P2P, Proxy, Others).

## Non-goals / constraints

- **Public contract preserved**: `PreferencesDialog(const AppSettings&, QWidget*)`
  and `AppSettings result() const` keep the same semantics.
- **Test hooks preserved**: `setConcurrentForTest`, `setMaxKBpsForTest`,
  `setUserAgentCustomForTest`, `setBrowserEnabledForTest`,
  `setStartAtLoginForTest` keep working → `tests/tst_settings.cpp` passes
  unchanged.
- No QSS. Placeholder dimmed text uses `QPalette::PlaceholderText`/`Disabled`.

## Architecture

The dialog's root layout changes from
`QVBoxLayout(QTabWidget, note, buttonBox)` to:

```
QVBoxLayout (root)
├── QHBoxLayout (body)
│   ├── QListWidget    (categories, fixed width ~160px)
│   └── QStackedWidget (one page per category)
└── QDialogButtonBox   (Restore defaults | OK | Cancel)
```

Each stack page is a `QWidget` with:

```
QVBoxLayout
├── header: QLabel (category name, bold) + QFrame (separator line)
└── QFormLayout with the category widgets  (or, for placeholders,
    a single centered dimmed QLabel)
```

Synchronization:
`connect(list, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex)`.

The existing member widgets (`m_concurrent`, `m_maxKBps`, `m_userAgent`,
`m_browserEnabled`, `m_startAtLogin`, etc.) stay as class members; only the
parent/page they are inserted into changes. `result()` keeps reading from the
same pointers.

### Initial category selection

New public method `void setInitialCategory(Category)` (enum) that selects the
matching row in the `QListWidget`. Default = first category (General). Used by
`MainWindow` to open directly on the Scheduler category.

## Categories

Order and content:

| # | Category | Content | Current origin |
|---|---|---|---|
| 1 | **General** | Start Orbit at login; Clipboard monitor (Off/Ask/Auto/Notify) | General tab (partial) |
| 2 | **Location** | Default download folder + Browse… button | General tab (folder) |
| 3 | **Downloads/Connection** | Simultaneous downloads; Segments per download; User-Agent (preset + custom); Connect timeout; Idle timeout; Max segment retries; Retry backoff base; Min segment size; Progress throttle | General + Advanced tabs |
| 4 | **Limits** | Max speed KB/s (0 = Unlimited) | General tab (max speed) |
| 5 | **Appearance** | Theme (System/Light/Dark) | Appearance tab |
| 6 | **Monitoring** | Browser bridge: enable; port; token (Copy/Regenerate) + 127.0.0.1:<port> hint | Browser tab |
| 7 | **Scheduler** | enabled; start (QTimeEdit); stop (QTimeEdit); recurrence (QComboBox); quit-when-done | moved from SchedulerDialog |
| 8 | **P2P Network** | placeholder "coming soon" | roadmap (P2P planned) |
| 9 | **Proxy** | placeholder "coming soon" | future |
| 10 | **Others** | placeholder "coming soon" | future |

## Scheduler migration

- The Scheduler widgets are now built on the "Scheduler" page of
  `PreferencesDialog`, populated from `m_base.scheduler`.
- `result()` now fills `out.scheduler` from those widgets. Today `result()`
  already preserves `m_base.scheduler` by copy; that field now becomes editable
  from the UI.
- `SchedulerDialog.cpp` and `SchedulerDialog.h` are **removed** from the
  `orbitgui` target and from `src/gui/CMakeLists.txt`.
- In `MainWindow`:
  - The existing action that opened `SchedulerDialog` now opens
    `PreferencesDialog` with `setInitialCategory(Category::Scheduler)`.
  - `applyPreferencesResult` already persists the whole `AppSettings`, including
    `scheduler`; verify that any runtime scheduling is re-applied from
    `result().scheduler` as it was in the `SchedulerDialog` flow.
- Check for tests referencing `SchedulerDialog` directly; migrate or remove as
  needed.

## Restore defaults

Button with role `QDialogButtonBox::ResetRole`. When clicked, it rebuilds the
widget values from a default `AppSettings{}` (the same defaults `SettingsIo`
would apply), **without closing the dialog** and **without persisting**.
Persistence happens only on OK, via `applyPreferencesResult` in `MainWindow`.

## Placeholders

Each "coming soon" category is a `QWidget` whose page contains only a centered
`QLabel` with dimmed text (e.g. "This section is coming soon."), using
`QPalette::PlaceholderText`. No interaction, no QSS.

## Error handling / edge cases

- Initial selection out of range → clamp to the first category.
- Placeholders have no editable widgets → `result()` reads nothing from them;
  the corresponding `AppSettings` fields are preserved from `m_base`.
- Restore defaults must not leave widgets in an inconsistent state (e.g. the
  User-Agent custom field must re-enable/disable according to the preset).

## Testing (TDD)

Kept:
- `tests/tst_settings.cpp` (test hooks + `result()` round-trip).

Added:
- `result().scheduler` reflects the Scheduler widget values after editing via
  new test hooks (e.g. `setSchedulerEnabledForTest`,
  `setSchedulerRecurrenceForTest`) — hooks only if needed to drive the widgets
  headless.
- `setInitialCategory(cat)` selects the correct page/row.
- Restore defaults resets the widgets to `AppSettings{}` default values.

## File impact

- `src/gui/PreferencesDialog.h` — `Category` enum, `setInitialCategory`, new
  Scheduler widget members, possible test hooks.
- `src/gui/PreferencesDialog.cpp` — layout rewrite (list+stack), pages, headers,
  footer with Restore, Scheduler migration, placeholders.
- `src/gui/SchedulerDialog.{h,cpp}` — removed.
- `src/gui/CMakeLists.txt` — remove `SchedulerDialog.cpp` from the target.
- `src/gui/MainWindow.cpp` — redirect the Scheduler action; remove the
  `SchedulerDialog` include and usage.
- `tests/` — adjustments/additions as above.

# Background Mode + Autostart — Design

Date: 2026-07-21
Status: Approved (pending spec review)

## Summary

Turn `orbit-gui` into a background-capable app without changing its single-process
architecture. Three related capabilities:

1. **Close-to-tray lifecycle** — closing the main window (X) no longer quits the app. The
   window hides to the system tray and the engine (`DownloadManager`), browser bridge
   (`BrowserBridge`) and `Scheduler` keep running. A one-time tray balloon explains this on the
   first close. Real quit happens only via a tray-menu **Quit** or **File > Quit**.
2. **Start at login (autostart)** — an opt-in **"Start Orbit at login"** toggle in Preferences.
   When on, the app is registered to launch automatically at login, starting **hidden in the
   tray** (no window, no focus stealing). macOS only for now, behind a generic interface so
   Windows/Linux can be added later without rework.
3. **Single instance** — a second launch (typical case: autostart already runs in the
   background, then the user opens the app manually) does not spawn a competing process. It
   asks the existing instance to show its window and exits, avoiding a `BrowserBridge` port
   clash and double session writes.

The `daemon` requested by the user is realized as **the same process kept alive in the tray**
(the qBittorrent/Transmission model), **not** a separate headless service. A separate daemon +
IPC was explicitly considered and rejected as unnecessary work/risk for this project.

## Background / Current State

- **Process model** (`src/gui/main_gui.cpp`): one `QApplication`. Builds `DownloadManager`,
  `DownloadTableModel`, `MainWindow`, calls `w.show()` then `app.exec()`. No CLI parsing.
- **Window lifecycle** (`src/gui/MainWindow.cpp`): no `closeEvent` override, so closing the
  last window quits (`QApplication::quitOnLastWindowClosed` defaults to `true`). **File > Quit**
  already calls `qApp->quit()`.
- **Tray** (`MainWindow.cpp:156-162`): a `QSystemTrayIcon* m_tray` already exists, used only for
  completion / bridge notifications. `messageClicked` opens the last completed file. **No
  context menu, no activation-to-show, no hide-to-tray.** `QSystemTrayIcon` is created even
  under the offscreen test platform (show/showMessage are no-ops there — tests rely on this).
- **Browser bridge** (`src/gui/BrowserBridge.*`): a `QTcpServer` on `127.0.0.1:<port>` owned by
  `MainWindow`; lives as long as the process does (not tied to window visibility). Today, since
  closing the window ends the process, the bridge dies with the window — the exact problem this
  work fixes.
- **Scheduler** (`MainWindow.cpp:684-691`): `quitWhenDone` calls `qApp->quit()` explicitly when
  all tasks complete. This keeps working unchanged in the new model.
- **Settings** (`src/gui/Settings.h`): `AppSettings { EngineConfig; UiPrefs; SchedulerConfig;
  BrowserPrefs; }`. `UiPrefs { defaultDownloadDir; clipboardMode; theme; }`. Persisted to
  `settings.json` via `SettingsIo::load/save` and `toJson/fromJson`.
- **Preferences** (`src/gui/PreferencesDialog.cpp`): a `QTabWidget` with **Appearance /
  General / Advanced / Browser** tabs. The **General** tab is where the autostart toggle lands.
- **Bundle identity** (`src/gui/CMakeLists.txt`): `MACOSX_BUNDLE_GUI_IDENTIFIER =
  dev.rootless.orbit-downloader-tribute`. Used to name the LaunchAgent label/plist.
- **Data dir**: `QStandardPaths::AppDataLocation + "/orbit-gui"`.

## Goals

- Closing the window keeps the process (and all downloads/bridge) alive; the app is reachable
  from the tray.
- The user can enable/disable "Start Orbit at login" from Preferences; it is **off by default**.
- Autostart launches the app hidden in the tray, with the engine and bridge already active.
- Two launches never fight over the bridge port or the session file.

## Non-Goals (YAGNI)

- A separate headless daemon process + IPC protocol.
- Windows/Linux autostart implementations (the interface is defined so they plug in later).
- Hiding the macOS Dock icon / `LSUIElement` background activation policy.
- A "close quits vs. minimizes" preference — closing **always** minimizes to tray (decided).
- Changing what the tray notifications already do (completion / bridge warnings stay as-is).

## Design

### 1. Close-to-tray lifecycle (`MainWindow`)

- In `main_gui.cpp`, after constructing the app: `app.setQuitOnLastWindowClosed(false)`. From
  then on the app never self-terminates just because no window is visible.
- Override `void MainWindow::closeEvent(QCloseEvent* e)`:
  - `e->ignore(); hide();`
  - If `!m_settings.ui.closeToTrayHintShown`: `m_tray->showMessage(tr("Orbit"), tr("Orbit is
    still running in the tray. Use the tray icon to reopen or quit."), Information, 5000)`, set
    `closeToTrayHintShown = true`, and persist via `SettingsIo::save` (guarded by non-empty
    `m_settingsPath`, matching existing save sites).
- **Tray context menu** (built once where the tray is created, `MainWindow.cpp:156`): a `QMenu`
  with **"Show Orbit"** and **"Quit"** (separator between). Assign with `m_tray->setContextMenu`.
  - "Show Orbit" → a new `showAndRaise()` helper: `showNormal(); raise();
    activateWindow();`.
  - "Quit" → a single `quitApp()` helper → `qApp->quit()`.
- `connect(m_tray, &QSystemTrayIcon::activated, ...)`: on `Trigger`/`DoubleClick` call
  `showAndRaise()`. (macOS delivers `Trigger`; keep it tolerant of both.)
- **File > Quit** (`MainWindow.cpp:184`) is repointed from the inline `qApp->quit()` lambda to
  `quitApp()` so both quit paths are identical.
- The existing `messageClicked` (open last completed file) is unchanged.

### 2. Background boot flag (`main_gui.cpp`)

- Parse args with `QCommandLineParser`: add a `--background` option (no value).
- If `--background` is **absent** → `w.show()` (today's behavior).
- If **present** → do **not** call `show()`. The window object is fully constructed (so the
  bridge, scheduler, model are all live) but starts hidden; only the tray is visible.
- The flag is passed **only** by the autostart mechanism. Manual launches never pass it.

### 3. `AutostartService` (generic interface + macOS impl)

New unit `src/gui/AutostartService.{h,cpp}`:

```cpp
class AutostartService {
public:
    virtual ~AutostartService() = default;
    virtual bool isEnabled() const = 0;      // reflects on-disk reality
    virtual bool setEnabled(bool on) = 0;    // returns success
    static std::unique_ptr<AutostartService> create();  // platform factory
};
```

- **macOS impl** (`MacAutostartService`): manages a per-user LaunchAgent.
  - Path: `~/Library/LaunchAgents/dev.rootless.orbit-downloader-tribute.plist`.
  - `setEnabled(true)`: **write the plist only** — do **not** `launchctl load` it. The agent
    takes effect at the next login (session launchd loads `~/Library/LaunchAgents` on login).
    We deliberately skip an immediate load: `RunAtLoad=true` would otherwise spawn a
    `--background` process the instant the user ticks the box (surprise launch). Effect is
    "starts next login", which is exactly what the toggle promises.
    - `Label` = `dev.rootless.orbit-downloader-tribute`.
    - `ProgramArguments` = `[<absolute executable path>, "--background"]`, where the path is
      resolved from `QCoreApplication::applicationFilePath()` (points inside `Orbit.app` for a
      bundle, or the plain binary otherwise).
    - `RunAtLoad` = `true`. No `KeepAlive` (we do not resurrect after an explicit Quit).
  - `setEnabled(false)`: best-effort `launchctl unload -w <path>` **only if currently loaded**
    (harmless otherwise), then remove the plist.
  - `isEnabled()`: the plist file exists.
  - Pure Qt file I/O + `QProcess` for the best-effort `launchctl unload`; **no `.mm`, no new
    dependency.**
- **Fallback impl** (non-macOS, for now): a no-op that reports `isEnabled() == false` and returns
  `false` from `setEnabled` — the toggle is simply hidden/disabled off-macOS (see §4).

`UiPrefs` gains **`bool startAtLogin = false;`** as the persisted intent. On boot, `main_gui`
reconciles once: if `startAtLogin != AutostartService::isEnabled()`, call `setEnabled` to match
the saved intent (covers a plist deleted out-of-band, or settings copied to a new machine).

### 4. Preferences toggle (General tab)

- Add a `QCheckBox` **"Start Orbit at login"** to the **General** tab in `PreferencesDialog`.
- Its checked state initializes from `AutostartService::isEnabled()` (ground truth), falling back
  to `UiPrefs::startAtLogin`.
- On Preferences **OK** (`MainWindow::onPreferences`): compare the new value to current; if
  changed, call `autostart->setEnabled(v)`, store the result in `m_settings.ui.startAtLogin`,
  and persist settings. If `setEnabled` fails, revert the stored flag and show a tray warning.
- On platforms where `AutostartService::create()` returns the no-op impl, hide (or disable with a
  tooltip) the checkbox so it never lies.

### 5. Single instance

- On startup, before binding anything, attempt `QLocalSocket::connectToServer(name)` where
  `name` is `orbit-gui-single` namespaced with the data dir hash (avoids collisions across
  installs).
  - **Connected** → an instance already runs: write a one-line message and `return 0` without
    constructing `MainWindow` (so no bridge/port bind, no session write). The message is
    **`"show"` for a foreground launch** and **`"ping"` for a `--background` launch** — a
    background secondary must **not** pop the window.
  - **Not connected** → we are the primary: `QLocalServer::removeServer(name)` (clear a stale
    socket), then `listen(name)`. On `newConnection`, read the message: `"show"` →
    `w.showAndRaise()`; `"ping"` → do nothing (the primary is already alive).
- This makes "autostart in background, then user double-clicks the app" resolve to *reveal the
  running instance*, while a stray second `--background` launch is absorbed silently. It also
  protects the session file and the bridge port generally.

## Data / Settings changes

`UiPrefs` (in `src/gui/Settings.h`) gains two persisted booleans:

- `bool startAtLogin = false;` — user intent for autostart (reconciled with `AutostartService`).
- `bool closeToTrayHintShown = false;` — whether the one-time close-to-tray balloon was shown.

Both are round-tripped in `SettingsIo::toJson`/`fromJson` alongside the existing `UiPrefs`
fields, defaulting to `false` when absent (backward compatible with existing `settings.json`).

## Error handling

- **LaunchAgent write/launchctl failure** → `setEnabled` returns `false`; the UI reverts the
  checkbox/flag and shows a tray warning. Settings are not left claiming an autostart that isn't
  registered.
- **Plist deleted out-of-band** → next-boot reconciliation (or reopening Preferences, which reads
  `isEnabled()`) restores consistency.
- **`QLocalServer::listen` fails as primary** → log and continue as a normal single window
  (degrade gracefully; single-instance is an optimization, not a correctness gate for the engine).
- **Tray unavailable** (offscreen/no tray): `showMessage` is already a safe no-op; hide-to-tray
  still hides the window (the process stays alive), consistent with current tray handling.

## Testing

Follow the existing QtTest + offscreen-platform patterns (`tests/tst_gui.cpp`).

- **closeEvent hides, does not quit**: send a close event; assert the window is hidden and the
  app is still running (window object alive).
- **One-time hint**: first close sets `closeToTrayHintShown`; a second close does not re-trigger
  the persisted flag transition. (Balloon itself is a no-op offscreen; assert the flag/persist.)
- **`--background` parsing**: with the flag, the boot path does not call `show()`; without it,
  it does. Factor the show-decision into a testable free function
  (`bool shouldStartHidden(const QStringList& args)`).
- **`AutostartService` (macOS)**: `setEnabled(true)` writes a plist whose `ProgramArguments`
  contains the executable path and `--background` and `RunAtLoad=true`; `setEnabled(false)`
  removes it; `isEnabled()` reflects file presence. Use a temp `HOME`/LaunchAgents dir so the
  test never touches the real one, and stub/skip the `launchctl` call under test (guarded like
  the existing `ORBIT_SKIP_TIMING_TESTS` quarantine if it proves environment-sensitive).
- **Single instance**: a second `connectToServer` succeeds against a listening primary and the
  secondary path returns without constructing a window.

## Rollout / files touched

- `src/gui/AutostartService.h` / `AutostartService.cpp` (new) — interface + macOS impl + factory.
- `src/gui/main_gui.cpp` — `setQuitOnLastWindowClosed(false)`, `QCommandLineParser`,
  single-instance guard, autostart reconciliation, hidden-vs-shown boot.
- `src/gui/MainWindow.{h,cpp}` — `closeEvent`, tray context menu, `showAndRaise()`, `quitApp()`,
  repoint File > Quit, `activated` handler.
- `src/gui/Settings.h` + `SettingsIo` (`toJson`/`fromJson`) — new `startAtLogin`,
  `closeToTrayHintShown`.
- `src/gui/PreferencesDialog.{h,cpp}` — "Start Orbit at login" checkbox in General.
- `src/gui/CMakeLists.txt` + `tests/CMakeLists.txt` — build the new unit and its tests.

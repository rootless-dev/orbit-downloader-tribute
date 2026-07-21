# Background Mode + Autostart Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep `orbit-gui` alive in the system tray when its window is closed, let the user opt into starting it hidden at login, and prevent a second launch from fighting the running one.

**Architecture:** Stays a single Qt process (no separate daemon). New small, headless-testable units land in `orbitgui_logic` (`SingleInstanceGuard`, `AutostartService`, a `--background` boot helper); the window lifecycle (close-to-tray, tray menu) and the Preferences toggle are surgical edits in `orbitgui`; `main_gui.cpp` wires them together.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, Test), CMake, QtTest. macOS-first (LaunchAgent plist for autostart).

## Global Constraints

- **Commit messages:** English, Conventional Commits (`feat:`, `fix:`, `docs:`, `test:`…). **Never** add `Co-Authored-By`. **Never commit without the user's explicit authorization** — the commit steps below are prepared, but the executor must have a go-ahead.
- **Autostart scope:** macOS only for now; the interface must be generic so Windows/Linux plug in later. Non-macOS builds get a no-op impl.
- **Autostart is opt-in:** default off. Enabling only writes the LaunchAgent plist (no immediate `launchctl load` — that would surprise-spawn a `--background` process).
- **Bundle identifier / LaunchAgent label:** `dev.rootless.orbit-downloader-tribute`.
- **Background flag:** `--background`. Passed only by the LaunchAgent; manual launches never pass it.
- **Test platform:** GUI tests run under `QT_QPA_PLATFORM=offscreen`. Autostart/single-instance tests must be hermetic — never touch the real `~/Library/LaunchAgents` or a real login item.
- **Backward compatibility:** new `settings.json` keys default to `false` when absent.

## File Structure

- `src/gui/Settings.h` / `Settings.cpp` (modify) — add `UiPrefs::startAtLogin` and `UiPrefs::closeToTrayHintShown`; round-trip them.
- `src/gui/BootOptions.h` / `BootOptions.cpp` (new, in `orbitgui_logic`) — `shouldStartHidden(const QStringList&)` and the `--background` option name.
- `src/gui/SingleInstanceGuard.h` / `SingleInstanceGuard.cpp` (new, in `orbitgui_logic`) — primary/secondary election over `QLocalServer`.
- `src/gui/AutostartService.h` / `AutostartService.cpp` (new, in `orbitgui_logic`) — interface, macOS LaunchAgent impl, no-op impl, factory.
- `src/gui/MainWindow.h` / `MainWindow.cpp` (modify) — `closeEvent`, tray context menu, `showAndRaise()`, `quitApp()`, one-time hint, `AutostartService` hook, apply-on-OK.
- `src/gui/PreferencesDialog.h` / `PreferencesDialog.cpp` (modify) — "Start Orbit at login" checkbox in the General tab.
- `src/gui/main_gui.cpp` (modify) — `setQuitOnLastWindowClosed(false)`, single-instance guard, background flag, service creation + reconciliation, inject service into `MainWindow`.
- `src/gui/CMakeLists.txt` (modify) — add new sources to `orbitgui_logic`.
- `tests/CMakeLists.txt` (modify) — add `tst_bootoptions`, `tst_singleinstance`, `tst_autostart`.
- `tests/tst_settings.cpp` (modify) — cover new fields.
- `tests/tst_bootoptions.cpp`, `tests/tst_singleinstance.cpp`, `tests/tst_autostart.cpp` (new).
- `tests/tst_gui.cpp` (modify) — close-to-tray + Preferences-toggle + autostart-wiring behavior.

**Build/test note:** the repo has a configured `build/` dir. Build a single test target with `cmake --build build --target <name>` and run it with `ctest --test-dir build -R <regex> -V`.

---

### Task 1: Persist `startAtLogin` and `closeToTrayHintShown`

**Files:**
- Modify: `src/gui/Settings.h:18-22` (UiPrefs struct)
- Modify: `src/gui/Settings.cpp:42-45` (fromJson) and `src/gui/Settings.cpp:66-69` (toJson)
- Test: `tests/tst_settings.cpp`

**Interfaces:**
- Produces: `UiPrefs::startAtLogin` (bool, default false), `UiPrefs::closeToTrayHintShown` (bool, default false), round-tripped under the `"ui"` JSON object as `"startAtLogin"` and `"closeToTrayHintShown"`.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_settings.cpp` inside the `appSettingsRoundTripThroughFile` slot (after the existing `s.ui.clipboardMode = ...` line and its assertions), or as a new slot. New slot:

```cpp
void uiFlagsRoundTrip() {
    QTemporaryDir dir;
    const QString path = dir.filePath("settings.json");
    AppSettings s;
    s.ui.startAtLogin = true;
    s.ui.closeToTrayHintShown = true;
    SettingsIo::save(path, s);
    const AppSettings back = SettingsIo::load(path, EngineConfig{});
    QCOMPARE(back.ui.startAtLogin, true);
    QCOMPARE(back.ui.closeToTrayHintShown, true);
}
void uiFlagsDefaultFalseWhenAbsent() {
    // A settings blob with a "ui" object lacking the new keys -> defaults false.
    const QJsonObject root{{"ui", QJsonObject{{"theme", "dark"}}}};
    const AppSettings s = SettingsIo::fromJson(root, EngineConfig{});
    QCOMPARE(s.ui.startAtLogin, false);
    QCOMPARE(s.ui.closeToTrayHintShown, false);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R settings -V`
Expected: FAIL to compile — `startAtLogin` / `closeToTrayHintShown` are not members of `UiPrefs`.

- [ ] **Step 3: Add the fields to UiPrefs**

In `src/gui/Settings.h`, extend `UiPrefs`:

```cpp
struct UiPrefs {
    QString       defaultDownloadDir;                 // vazio => boot resolve p/ Downloads
    ClipboardMode clipboardMode = ClipboardMode::Off;
    ThemePref     theme         = ThemePref::System;
    bool          startAtLogin        = false;        // intent: launch hidden at login
    bool          closeToTrayHintShown = false;       // one-time "still running in tray" balloon
};
```

- [ ] **Step 4: Round-trip in Settings.cpp**

In `src/gui/Settings.cpp`, in `fromJson` after line 45 (`s.ui.theme = ...`):

```cpp
    s.ui.startAtLogin        = ui.value("startAtLogin").toBool(false);
    s.ui.closeToTrayHintShown = ui.value("closeToTrayHintShown").toBool(false);
```

In `toJson`, extend the `root["ui"]` object:

```cpp
    root["ui"]       = QJsonObject{
        {"defaultDownloadDir", s.ui.defaultDownloadDir},
        {"clipboardMode",      clipToStr(s.ui.clipboardMode)},
        {"theme",              themeToStr(s.ui.theme)},
        {"startAtLogin",       s.ui.startAtLogin},
        {"closeToTrayHintShown", s.ui.closeToTrayHintShown}};
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R settings -V`
Expected: PASS (all slots, including the two new ones).

- [ ] **Step 6: Commit**

```bash
git add src/gui/Settings.h src/gui/Settings.cpp tests/tst_settings.cpp
git commit -m "feat(settings): persist startAtLogin and closeToTrayHintShown"
```

---

### Task 2: `--background` boot helper (`BootOptions`)

**Files:**
- Create: `src/gui/BootOptions.h`, `src/gui/BootOptions.cpp`
- Modify: `src/gui/CMakeLists.txt:2-12` (add to `orbitgui_logic`)
- Create: `tests/tst_bootoptions.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `constexpr char kBackgroundFlag[] = "--background";` and `bool shouldStartHidden(const QStringList& args);` (returns true iff `args` contains `--background`).

- [ ] **Step 1: Write the failing test**

Create `tests/tst_bootoptions.cpp`:

```cpp
#include <QtTest>
#include "BootOptions.h"

class TstBootOptions : public QObject {
    Q_OBJECT
private slots:
    void hiddenWhenFlagPresent() {
        QVERIFY(shouldStartHidden({"orbit", "--background"}));
    }
    void visibleWhenFlagAbsent() {
        QVERIFY(!shouldStartHidden({"orbit"}));
        QVERIFY(!shouldStartHidden({"orbit", "--other"}));
    }
};
QTEST_APPLESS_MAIN(TstBootOptions)
#include "tst_bootoptions.moc"
```

- [ ] **Step 2: Register the test target and run to verify it fails**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_bootoptions tst_bootoptions.cpp)
target_link_libraries(tst_bootoptions PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_bootoptions COMMAND tst_bootoptions)
```

Run: `cmake --build build --target tst_bootoptions`
Expected: FAIL — `BootOptions.h` not found.

- [ ] **Step 3: Create BootOptions**

Create `src/gui/BootOptions.h`:

```cpp
#pragma once
#include <QStringList>

// The CLI flag that tells the app to start hidden in the tray (passed only by
// the autostart LaunchAgent). Manual launches never pass it.
inline constexpr char kBackgroundFlag[] = "--background";

// True iff the app was asked to start hidden in the tray.
bool shouldStartHidden(const QStringList& args);
```

Create `src/gui/BootOptions.cpp`:

```cpp
#include "BootOptions.h"

bool shouldStartHidden(const QStringList& args) {
    return args.contains(QLatin1String(kBackgroundFlag));
}
```

- [ ] **Step 4: Add BootOptions.cpp to orbitgui_logic**

In `src/gui/CMakeLists.txt`, add `BootOptions.cpp` to the `orbitgui_logic` source list (after `DropTargets.cpp`):

```cmake
    DropTargets.cpp
    BootOptions.cpp
    Settings.cpp
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target tst_bootoptions && ctest --test-dir build -R bootoptions -V`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gui/BootOptions.h src/gui/BootOptions.cpp src/gui/CMakeLists.txt tests/tst_bootoptions.cpp tests/CMakeLists.txt
git commit -m "feat(boot): add --background start-hidden helper"
```

---

### Task 3: `SingleInstanceGuard`

**Files:**
- Create: `src/gui/SingleInstanceGuard.h`, `src/gui/SingleInstanceGuard.cpp`
- Modify: `src/gui/CMakeLists.txt` (add to `orbitgui_logic`)
- Create: `tests/tst_singleinstance.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `constexpr char kShowMessage[] = "show";` and `constexpr char kPingMessage[] = "ping";`
  - `class SingleInstanceGuard : public QObject` with:
    - `explicit SingleInstanceGuard(QString serverName, QObject* parent = nullptr);`
    - `bool tryBecomePrimary(const QByteArray& secondaryMessage);` — returns `true` and starts listening if no primary exists; otherwise sends `secondaryMessage` to the existing primary and returns `false`.
    - signal `void showRequested();` — emitted on the primary when a secondary sends `kShowMessage` (never for `kPingMessage`).

- [ ] **Step 1: Write the failing test**

Create `tests/tst_singleinstance.cpp`:

```cpp
#include <QtTest>
#include <QLocalSocket>
#include <QSignalSpy>
#include "SingleInstanceGuard.h"

class TstSingleInstance : public QObject {
    Q_OBJECT
    QString name() const { return "orbit-test-" + QString::number(quintptr(this)); }
private slots:
    void firstBecomesPrimary() {
        SingleInstanceGuard g(name());
        QVERIFY(g.tryBecomePrimary(kShowMessage));   // nobody else -> primary
    }
    void secondIsSecondaryAndSignalsShow() {
        const QString n = name();
        SingleInstanceGuard primary(n);
        QVERIFY(primary.tryBecomePrimary(kPingMessage));
        QSignalSpy spy(&primary, &SingleInstanceGuard::showRequested);
        SingleInstanceGuard second(n);
        QVERIFY(!second.tryBecomePrimary(kShowMessage));  // primary exists
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
    }
    void pingDoesNotSignalShow() {
        const QString n = name();
        SingleInstanceGuard primary(n);
        QVERIFY(primary.tryBecomePrimary(kPingMessage));
        QSignalSpy spy(&primary, &SingleInstanceGuard::showRequested);
        SingleInstanceGuard second(n);
        QVERIFY(!second.tryBecomePrimary(kPingMessage));  // background secondary
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);
    }
};
QTEST_MAIN(TstSingleInstance)
#include "tst_singleinstance.moc"
```

- [ ] **Step 2: Register the test target and run to verify it fails**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_singleinstance tst_singleinstance.cpp)
target_link_libraries(tst_singleinstance PRIVATE orbitgui_logic Qt6::Test Qt6::Network)
add_test(NAME tst_singleinstance COMMAND tst_singleinstance)
```

Run: `cmake --build build --target tst_singleinstance`
Expected: FAIL — `SingleInstanceGuard.h` not found.

- [ ] **Step 3: Create SingleInstanceGuard header**

Create `src/gui/SingleInstanceGuard.h`:

```cpp
#pragma once
#include <QObject>
#include <QString>
class QLocalServer;

inline constexpr char kShowMessage[] = "show";   // foreground second launch -> reveal window
inline constexpr char kPingMessage[] = "ping";   // background second launch -> just exit

class SingleInstanceGuard : public QObject {
    Q_OBJECT
public:
    explicit SingleInstanceGuard(QString serverName, QObject* parent = nullptr);
    // No primary yet -> start listening and return true. Otherwise send
    // secondaryMessage to the existing primary and return false.
    bool tryBecomePrimary(const QByteArray& secondaryMessage);
signals:
    void showRequested();
private:
    void onNewConnection();
    QString       m_name;
    QLocalServer* m_server = nullptr;
};
```

- [ ] **Step 4: Create SingleInstanceGuard implementation**

Create `src/gui/SingleInstanceGuard.cpp`:

```cpp
#include "SingleInstanceGuard.h"
#include <QLocalServer>
#include <QLocalSocket>

SingleInstanceGuard::SingleInstanceGuard(QString serverName, QObject* parent)
    : QObject(parent), m_name(std::move(serverName)) {}

bool SingleInstanceGuard::tryBecomePrimary(const QByteArray& secondaryMessage) {
    // Probe for an existing primary.
    QLocalSocket probe;
    probe.connectToServer(m_name);
    if (probe.waitForConnected(200)) {
        probe.write(secondaryMessage);
        probe.flush();
        probe.waitForBytesWritten(200);
        probe.disconnectFromServer();
        return false;                          // a primary already runs
    }
    // Become the primary. removeServer clears a stale socket left by a crash.
    QLocalServer::removeServer(m_name);
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this,
            &SingleInstanceGuard::onNewConnection);
    m_server->listen(m_name);                  // best-effort; failure -> no IPC, still primary
    return true;
}

void SingleInstanceGuard::onNewConnection() {
    while (QLocalSocket* sock = m_server->nextPendingConnection()) {
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] {
            const QByteArray msg = sock->readAll();
            if (msg.contains(kShowMessage))
                emit showRequested();
            sock->disconnectFromServer();
            sock->deleteLater();
        });
        connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
    }
}
```

- [ ] **Step 5: Add SingleInstanceGuard.cpp to orbitgui_logic**

In `src/gui/CMakeLists.txt`, add `SingleInstanceGuard.cpp` to the `orbitgui_logic` source list:

```cmake
    BootOptions.cpp
    SingleInstanceGuard.cpp
    Settings.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build --target tst_singleinstance && ctest --test-dir build -R singleinstance -V`
Expected: PASS (3 slots).

- [ ] **Step 7: Commit**

```bash
git add src/gui/SingleInstanceGuard.h src/gui/SingleInstanceGuard.cpp src/gui/CMakeLists.txt tests/tst_singleinstance.cpp tests/CMakeLists.txt
git commit -m "feat(boot): add single-instance guard with show/ping IPC"
```

---

### Task 4: `AutostartService` (interface + macOS LaunchAgent + no-op + factory)

**Files:**
- Create: `src/gui/AutostartService.h`, `src/gui/AutostartService.cpp`
- Modify: `src/gui/CMakeLists.txt` (add to `orbitgui_logic`)
- Create: `tests/tst_autostart.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `class AutostartService` (abstract): `virtual bool isEnabled() const = 0;`, `virtual bool setEnabled(bool on) = 0;`, `static std::unique_ptr<AutostartService> create();`
  - `constexpr char kAutostartLabel[] = "dev.rootless.orbit-downloader-tribute";`
  - macOS only (guarded by `Q_OS_MACOS`): `class MacAutostartService : public AutostartService` with ctor `MacAutostartService(QString plistPath, QString execPath, bool useLaunchctl = true);` — used directly by tests with a temp path and `useLaunchctl = false`.

- [ ] **Step 1: Write the failing test**

Create `tests/tst_autostart.cpp`:

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "AutostartService.h"

class TstAutostart : public QObject {
    Q_OBJECT
#ifdef Q_OS_MACOS
private slots:
    void disabledByDefault() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        MacAutostartService s(plist, "/Applications/Orbit.app/Contents/MacOS/Orbit",
                              /*useLaunchctl=*/false);
        QVERIFY(!s.isEnabled());
    }
    void enableWritesPlistWithFlag() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        const QString exec  = "/Applications/Orbit.app/Contents/MacOS/Orbit";
        MacAutostartService s(plist, exec, /*useLaunchctl=*/false);
        QVERIFY(s.setEnabled(true));
        QVERIFY(s.isEnabled());
        QFile f(plist);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString xml = QString::fromUtf8(f.readAll());
        QVERIFY(xml.contains("dev.rootless.orbit-downloader-tribute"));  // Label
        QVERIFY(xml.contains(exec));                                     // ProgramArguments[0]
        QVERIFY(xml.contains("--background"));                           // ProgramArguments[1]
        QVERIFY(xml.contains("RunAtLoad"));
    }
    void disableRemovesPlist() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        MacAutostartService s(plist, "/x/Orbit", /*useLaunchctl=*/false);
        QVERIFY(s.setEnabled(true));
        QVERIFY(s.isEnabled());
        QVERIFY(s.setEnabled(false));
        QVERIFY(!s.isEnabled());
        QVERIFY(!QFile::exists(plist));
    }
#endif
    void factoryNeverNull() {
        QVERIFY(AutostartService::create() != nullptr);   // no-op off-macOS, Mac impl on-macOS
    }
};
QTEST_APPLESS_MAIN(TstAutostart)
#include "tst_autostart.moc"
```

- [ ] **Step 2: Register the test target and run to verify it fails**

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(tst_autostart tst_autostart.cpp)
target_link_libraries(tst_autostart PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_autostart COMMAND tst_autostart)
```

Run: `cmake --build build --target tst_autostart`
Expected: FAIL — `AutostartService.h` not found.

- [ ] **Step 3: Create AutostartService header**

Create `src/gui/AutostartService.h`:

```cpp
#pragma once
#include <QString>
#include <QtGlobal>
#include <memory>

inline constexpr char kAutostartLabel[] = "dev.rootless.orbit-downloader-tribute";

// Enables/disables "start the app at login". Ground truth lives on disk
// (a LaunchAgent plist on macOS), so isEnabled() reflects reality even if the
// file was changed out of band.
class AutostartService {
public:
    virtual ~AutostartService() = default;
    virtual bool isEnabled() const = 0;
    virtual bool setEnabled(bool on) = 0;
    static std::unique_ptr<AutostartService> create();   // platform factory
};

#ifdef Q_OS_MACOS
class MacAutostartService : public AutostartService {
public:
    // plistPath: ~/Library/LaunchAgents/<label>.plist in production.
    // execPath : absolute path to the running binary (bundle inner binary is fine).
    // useLaunchctl: false in tests to stay hermetic (no launchd side effects).
    MacAutostartService(QString plistPath, QString execPath, bool useLaunchctl = true);
    bool isEnabled() const override;
    bool setEnabled(bool on) override;
private:
    QString buildPlistXml() const;
    QString m_plistPath;
    QString m_execPath;
    bool    m_useLaunchctl;
};
#endif
```

- [ ] **Step 4: Create AutostartService implementation**

Create `src/gui/AutostartService.cpp`:

```cpp
#include "AutostartService.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace {
// No-op used on platforms without an autostart backend yet (Windows/Linux).
class NoopAutostartService : public AutostartService {
public:
    bool isEnabled() const override { return false; }
    bool setEnabled(bool) override { return false; }
};
}

#ifdef Q_OS_MACOS
#include <QProcess>

MacAutostartService::MacAutostartService(QString plistPath, QString execPath, bool useLaunchctl)
    : m_plistPath(std::move(plistPath)), m_execPath(std::move(execPath)),
      m_useLaunchctl(useLaunchctl) {}

bool MacAutostartService::isEnabled() const {
    return QFile::exists(m_plistPath);
}

QString MacAutostartService::buildPlistXml() const {
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>%1</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>%2</string>\n"
        "        <string>--background</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "</dict>\n"
        "</plist>\n")
        .arg(QLatin1String(kAutostartLabel), m_execPath.toHtmlEscaped());
}

bool MacAutostartService::setEnabled(bool on) {
    if (on) {
        QDir().mkpath(QFileInfo(m_plistPath).absolutePath());
        QSaveFile f(m_plistPath);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(buildPlistXml().toUtf8());
        return f.commit();
        // Deliberately no `launchctl load`: RunAtLoad would spawn a --background
        // process immediately. The agent takes effect at next login.
    }
    // Disable: best-effort unload (only meaningful if it was loaded), then remove.
    if (m_useLaunchctl && QFile::exists(m_plistPath))
        QProcess::execute("launchctl", {"unload", "-w", m_plistPath});
    if (QFile::exists(m_plistPath))
        return QFile::remove(m_plistPath);
    return true;
}
#endif  // Q_OS_MACOS

std::unique_ptr<AutostartService> AutostartService::create() {
#ifdef Q_OS_MACOS
    const QString plist = QDir::homePath()
        + "/Library/LaunchAgents/" + QLatin1String(kAutostartLabel) + ".plist";
    return std::make_unique<MacAutostartService>(plist,
                                                 QCoreApplication::applicationFilePath());
#else
    return std::make_unique<NoopAutostartService>();
#endif
}
```

- [ ] **Step 5: Add AutostartService.cpp to orbitgui_logic**

In `src/gui/CMakeLists.txt`, add `AutostartService.cpp` to the `orbitgui_logic` source list:

```cmake
    SingleInstanceGuard.cpp
    AutostartService.cpp
    Settings.cpp
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build --target tst_autostart && ctest --test-dir build -R autostart -V`
Expected: PASS (macOS: 4 slots; other platforms: only `factoryNeverNull`).

- [ ] **Step 7: Commit**

```bash
git add src/gui/AutostartService.h src/gui/AutostartService.cpp src/gui/CMakeLists.txt tests/tst_autostart.cpp tests/CMakeLists.txt
git commit -m "feat(autostart): add macOS LaunchAgent autostart service"
```

---

### Task 5: Close-to-tray lifecycle + tray menu (`MainWindow`)

**Files:**
- Modify: `src/gui/MainWindow.h:60-62` (add `closeEvent` decl), `:84-100` (add helper decls + test hooks), `:119-121` (members)
- Modify: `src/gui/MainWindow.cpp:45` (includes), `:156-162` (tray setup), `:184` (File > Quit repoint)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `m_settings.ui.closeToTrayHintShown` (Task 1), `m_tray`, `m_settingsPath`, `SettingsIo::save`.
- Produces: `void MainWindow::showAndRaise();`, `void MainWindow::quitApp();`, `protected void closeEvent(QCloseEvent*) override;`, test hook `bool closeToTrayHintShownForTest() const;`.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp` (a new slot in the existing test class; it constructs a `MainWindow` the same way other GUI slots do — reuse the existing helper/fixture pattern already in that file):

```cpp
void closeHidesToTrayAndDoesNotQuit() {
    QTemporaryDir dir;
    EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
    DownloadTableModel model(&mgr);
    Logger logger(dir.path());
    MainWindow w(&mgr, &model, &logger);
    w.applySettings(AppSettings{}, dir.filePath("settings.json"));
    w.show();
    QVERIFY(w.isVisible());
    QCloseEvent ev;
    QApplication::sendEvent(&w, &ev);
    QVERIFY(!ev.isAccepted());          // closeEvent ignored the event
    QVERIFY(!w.isVisible());            // hidden, not destroyed
    QVERIFY(w.closeToTrayHintShownForTest());  // one-time hint flag set
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target tst_gui`
Expected: FAIL — `closeToTrayHintShownForTest` undefined and `closeEvent` not overridden (window would try to close/quit).

- [ ] **Step 3: Declare the overrides, helpers, and hook in the header**

In `src/gui/MainWindow.h`, add the include-forward and declarations.

Under `protected:` (near line 60, alongside `dragEnterEvent`):

```cpp
    void closeEvent(QCloseEvent* e) override;
```

Add a forward declaration near the other `class Q...;` lines (top of file):

```cpp
class QCloseEvent;
```

In the `private:` helpers block (near line 84):

```cpp
    void    showAndRaise();
    void    quitApp();
```

In the public test-hooks block (near line 59):

```cpp
    bool closeToTrayHintShownForTest() const { return m_settings.ui.closeToTrayHintShown; }
```

- [ ] **Step 4: Implement closeEvent, helpers, and tray menu**

In `src/gui/MainWindow.cpp`, ensure includes near line 45:

```cpp
#include <QCloseEvent>
#include <QMenu>
#include <QSystemTrayIcon>
```

Replace the tray setup block (currently `MainWindow.cpp:156-162`) with a version that adds a context menu and activation-to-show:

```cpp
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(qApp->windowIcon());
    auto* trayMenu = new QMenu(this);
    QAction* aShow = trayMenu->addAction(tr("Show Orbit"));
    connect(aShow, &QAction::triggered, this, &MainWindow::showAndRaise);
    trayMenu->addSeparator();
    QAction* aTrayQuit = trayMenu->addAction(tr("Quit"));
    connect(aTrayQuit, &QAction::triggered, this, &MainWindow::quitApp);
    m_tray->setContextMenu(trayMenu);
    m_tray->show();
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
            showAndRaise();
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]{
        if (!m_lastCompletedPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastCompletedPath));
    });
```

Repoint File > Quit (currently `MainWindow.cpp:184`):

```cpp
    connect(aQuit, &QAction::triggered, this, &MainWindow::quitApp);
```

Add the three method bodies (anywhere sensible in the .cpp, e.g. near `maybeQuitWhenDone`):

```cpp
void MainWindow::closeEvent(QCloseEvent* e) {
    e->ignore();      // don't quit; hide to tray and keep the engine/bridge alive
    hide();
    if (!m_settings.ui.closeToTrayHintShown) {
        if (m_tray)
            m_tray->showMessage(tr("Orbit"),
                tr("Orbit is still running in the tray. Use the tray icon to reopen or quit."),
                QSystemTrayIcon::Information, 5000);
        m_settings.ui.closeToTrayHintShown = true;
        if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
    }
}

void MainWindow::showAndRaise() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitApp() {
    qApp->quit();
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R gui -V`
Expected: PASS, including `closeHidesToTrayAndDoesNotQuit`, with no regressions in the existing GUI slots.

- [ ] **Step 6: Commit**

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): hide main window to tray on close with one-time hint"
```

---

### Task 6: "Start Orbit at login" checkbox (`PreferencesDialog`)

**Files:**
- Modify: `src/gui/PreferencesDialog.h` (member + test hook)
- Modify: `src/gui/PreferencesDialog.cpp:39-84` (General tab), `:159-179` (result)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `AppSettings::ui.startAtLogin` (Task 1).
- Produces: `PreferencesDialog::result().ui.startAtLogin` reflects the checkbox; test hook `void setStartAtLoginForTest(bool on);`.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp`:

```cpp
void preferencesStartAtLoginRoundTrips() {
    AppSettings base;
    base.ui.startAtLogin = false;
    PreferencesDialog dlg(base);         // parent omitted: dialog owns itself for the test
    dlg.setStartAtLoginForTest(true);
    QCOMPARE(dlg.result().ui.startAtLogin, true);
}
```

Ensure `#include "PreferencesDialog.h"` is present in `tst_gui.cpp` (add if missing).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target tst_gui`
Expected: FAIL — `setStartAtLoginForTest` undefined.

- [ ] **Step 3: Add the member and hook to the header**

In `src/gui/PreferencesDialog.h`, add a `QCheckBox* m_startAtLogin = nullptr;` member (near the other widget members) and, in the public test-hooks area (next to `setBrowserEnabledForTest`):

```cpp
    void setStartAtLoginForTest(bool on);
```

Add `class QCheckBox;` forward declaration if the header does not already include/forward it.

- [ ] **Step 4: Add the checkbox to the General tab and wire result()**

In `src/gui/PreferencesDialog.cpp`, in the General tab construction (after the User-Agent rows, before `tabs->addTab(gen, tr("General"));` at line 84):

```cpp
    m_startAtLogin = new QCheckBox(tr("Start Orbit at login"), gen);
    m_startAtLogin->setChecked(current.ui.startAtLogin);
    gf->addRow(QString(), m_startAtLogin);
```

In `result()` (near line 172, alongside the other `s.ui.*` assignments):

```cpp
    s.ui.startAtLogin = m_startAtLogin->isChecked();
```

At the end of the file (next to the other `...ForTest` definitions):

```cpp
void PreferencesDialog::setStartAtLoginForTest(bool on) { m_startAtLogin->setChecked(on); }
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R gui -V`
Expected: PASS (`preferencesStartAtLoginRoundTrips` plus existing slots).

- [ ] **Step 6: Commit**

```bash
git add src/gui/PreferencesDialog.h src/gui/PreferencesDialog.cpp tests/tst_gui.cpp
git commit -m "feat(prefs): add Start Orbit at login checkbox"
```

---

### Task 7: Apply autostart on Preferences OK (`MainWindow`)

**Files:**
- Modify: `src/gui/MainWindow.h` (member + setter + include-forward)
- Modify: `src/gui/MainWindow.cpp:693-707` (`onPreferences`)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `AutostartService` (Task 4), `PreferencesDialog::result().ui.startAtLogin` (Task 6).
- Produces: `void MainWindow::setAutostartService(AutostartService* s);` — when set, Preferences OK calls `setEnabled` iff the value changed and mirrors the result into `m_settings.ui.startAtLogin`. When null (default, and in most GUI tests), autostart is skipped.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp` a fake service and a slot:

```cpp
class FakeAutostart : public AutostartService {
public:
    bool enabled = false;
    bool isEnabled() const override { return enabled; }
    bool setEnabled(bool on) override { enabled = on; return true; }
};

// ... in the test class:
void preferencesAppliesAutostart() {
    QTemporaryDir dir;
    EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
    DownloadTableModel model(&mgr);
    Logger logger(dir.path());
    MainWindow w(&mgr, &model, &logger);
    w.applySettings(AppSettings{}, dir.filePath("settings.json"));
    FakeAutostart fake;
    w.setAutostartService(&fake);
    // Drive the apply-on-OK body directly (no modal exec): startAtLogin flips false->true.
    AppSettings next;              // defaults + our change
    next.ui.startAtLogin = true;
    w.applyPreferencesResultForTest(next);
    QVERIFY(fake.enabled);         // service was toggled on
    QVERIFY(fake.isEnabled());
}
```

`FakeAutostart` is a file-scope helper in `tst_gui.cpp` (define it above the test class, near `makeTempDir`).

This needs a small test seam: a `applyPreferencesResultForTest(const AppSettings&)` hook that runs the same apply-on-OK body `onPreferences` uses. Add it in Step 3.

Add `#include "AutostartService.h"` to `tst_gui.cpp`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target tst_gui`
Expected: FAIL — `setAutostartService` / `applyPreferencesResultForTest` undefined.

- [ ] **Step 3: Add member, setter, forward-decl, and refactor onPreferences**

In `src/gui/MainWindow.h`:
- Forward declare near the other forwards: `class AutostartService;`
- Add member near `m_bridge`: `AutostartService* m_autostart = nullptr;`
- Add public setter and test hook:

```cpp
    void setAutostartService(AutostartService* s) { m_autostart = s; }
    void applyPreferencesResultForTest(const AppSettings& r) { applyPreferencesResult(r); }
```

- Add a private helper decl near `onPreferences`:

```cpp
    void applyPreferencesResult(const AppSettings& r);
```

In `src/gui/MainWindow.cpp`, add `#include "AutostartService.h"` near the other gui includes, then refactor `onPreferences` to extract the apply body so both the dialog and the test hook share it:

```cpp
void MainWindow::onPreferences() {
    PreferencesDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted) return;
    applyPreferencesResult(dlg.result());
}

void MainWindow::applyPreferencesResult(const AppSettings& r) {
    const bool wantAutostart = r.ui.startAtLogin;
    const bool hadAutostart  = m_settings.ui.startAtLogin;
    m_settings = r;
    applyTheme(m_settings.ui.theme);              // live theme (unchanged behavior)
    m_mgr->setConfig(m_settings.engine);
    applyBrowserBridge(m_settings.browser);
    m_clip->setMode(m_settings.ui.clipboardMode);
    if (m_clipGroup) {
        for (QAction* a : m_clipGroup->actions())
            if (a->data().toInt() == int(m_settings.ui.clipboardMode)) { a->setChecked(true); break; }
    }
    if (!m_settings.ui.defaultDownloadDir.isEmpty()) m_lastDir = m_settings.ui.defaultDownloadDir;

    if (m_autostart && wantAutostart != hadAutostart) {
        if (!m_autostart->setEnabled(wantAutostart)) {
            m_settings.ui.startAtLogin = hadAutostart;   // revert on failure
            if (m_tray)
                m_tray->showMessage(tr("Orbit"),
                    tr("Could not change the login item."),
                    QSystemTrayIcon::Warning, 5000);
        }
    }
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}
```

(This preserves every line the original `onPreferences` did — theme, engine, bridge, clipboard, dir, save — and adds the autostart apply.)

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R gui -V`
Expected: PASS (`preferencesAppliesAutostart` plus existing slots).

- [ ] **Step 5: Commit**

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): apply Start-at-login on Preferences OK"
```

---

### Task 8: Wire it all in `main_gui.cpp`

**Files:**
- Modify: `src/gui/main_gui.cpp` (entire boot sequence)

**Interfaces:**
- Consumes: `shouldStartHidden` (Task 2), `SingleInstanceGuard` + `kShowMessage`/`kPingMessage` (Task 3), `AutostartService::create` (Task 4), `MainWindow::showAndRaise`/`setAutostartService` (Tasks 5, 7).
- Produces: the running app's boot behavior. No unit test (integration glue) — verified by build + manual run in Step 3.

- [ ] **Step 1: Rewrite the boot sequence**

Replace the body of `src/gui/main_gui.cpp` with:

```cpp
#include <QApplication>
#include <QStandardPaths>
#include <QCryptographicHash>
#include "AutostartService.h"
#include "BootOptions.h"
#include "DownloadManager.h"
#include "DownloadTableModel.h"
#include "Logger.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SingleInstanceGuard.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("orbit-gui");
    QApplication::setQuitOnLastWindowClosed(false);   // live in the tray, not tied to the window

    const bool hidden = shouldStartHidden(app.arguments());

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-gui";

    // Single instance: a name namespaced by the data dir so parallel installs don't clash.
    const QString guardName = "orbit-gui-"
        + QString::fromLatin1(QCryptographicHash::hash(dataDir.toUtf8(),
                              QCryptographicHash::Sha1).toHex().left(12));
    SingleInstanceGuard guard(guardName);
    if (!guard.tryBecomePrimary(hidden ? kPingMessage : kShowMessage))
        return 0;   // another instance handles it (and shows itself if we were foreground)

    const QString settingsPath = dataDir + "/settings.json";
    AppSettings settings = SettingsIo::load(settingsPath, EngineConfig{});

    Logger logger(dataDir);
    DownloadManager mgr(settings.engine, dataDir, &logger);
    mgr.loadSession();                    // restore tasks BEFORE building the model
    DownloadTableModel model(&mgr);       // ctor seeds rows from mgr.tasks()

    auto autostart = AutostartService::create();
    // Reconcile persisted intent with on-disk reality (plist deleted out of band, etc.).
    if (settings.ui.startAtLogin != autostart->isEnabled())
        autostart->setEnabled(settings.ui.startAtLogin);

    MainWindow w(&mgr, &model, &logger);
    w.setAutostartService(autostart.get());
    w.applySettings(settings, settingsPath);
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(960, 640);
    if (!hidden) w.show();                // autostart (--background) stays hidden in the tray

    QObject::connect(&guard, &SingleInstanceGuard::showRequested,
                     &w, &MainWindow::showAndRaise);
    return app.exec();
}
```

- [ ] **Step 2: Build the app**

Run: `cmake --build build --target orbit-gui`
Expected: builds cleanly.

- [ ] **Step 3: Manual smoke verification**

Run the app (`build/.../Orbit.app` or the built binary). Verify, one by one:
1. Launch normally → window shows.
2. Close the window (X) → window hides, app keeps running (tray icon present), first close shows the "still running in the tray" balloon.
3. Tray menu → "Show Orbit" brings the window back; "Quit" exits.
4. Launch a second copy while the first runs (no `--background`) → the first window is revealed, no second process lingers.
5. Preferences → tick "Start Orbit at login" → OK → `~/Library/LaunchAgents/dev.rootless.orbit-downloader-tribute.plist` exists; untick → OK → it is removed.
6. Launch with `--background` → no window, only the tray; the browser bridge still accepts a handoff.

- [ ] **Step 4: Run the full test suite**

Run: `ctest --test-dir build`
Expected: all tests pass (no regressions).

- [ ] **Step 5: Commit**

```bash
git add src/gui/main_gui.cpp
git commit -m "feat(gui): background boot, single instance, and autostart wiring"
```

---

## Notes for the executor

- **Test fixture in `tst_gui.cpp`:** existing slots construct the window inline as `QTemporaryDir dir; EngineConfig cfg; DownloadManager mgr(cfg, dir.path()); DownloadTableModel model(&mgr); Logger logger(dir.path()); MainWindow w(&mgr, &model, &logger); w.applySettings(AppSettings{}, dir.filePath("settings.json"));` (see the `mainwindow_*` slots). The snippets above already follow that pattern.
- **macOS-only guards:** `AutostartService.cpp`'s Mac impl and `tst_autostart.cpp`'s Mac slots are under `#ifdef Q_OS_MACOS`. On non-macOS the factory returns the no-op and only `factoryNeverNull` runs — that's expected.
- **Do not** add `launchctl load` on enable; the next-login semantics are intentional (see Global Constraints).
- **Commit authorization:** stop and get the user's go-ahead before running any `git commit` step.

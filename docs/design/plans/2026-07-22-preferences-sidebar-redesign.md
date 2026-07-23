# Preferences Sidebar Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `PreferencesDialog` from a horizontal `QTabWidget` into the classic Orbit layout — a left category list plus a right content panel — and fold the standalone `SchedulerDialog` into it as a category.

**Architecture:** Inside `PreferencesDialog.cpp`, replace the tab widget with a `QHBoxLayout(QListWidget, QStackedWidget)` body and a footer `QDialogButtonBox` (Restore defaults | OK | Cancel). Each category is a stacked page with a bold header + separator line above its `QFormLayout`. The Scheduler widgets move onto a "Scheduler" page; the `SchedulerDialog` class is deleted and its toolbar/menu action reopens Preferences pre-selected on that page. Persistence (`Settings.*` / `SettingsIo`) and the JSON format are untouched.

**Tech Stack:** C++20, Qt 6 (Widgets), QTest. Native styling via `QPalette` — no QSS.

## Global Constraints

- Qt 6, C++20, CMake (targets `orbitgui_logic` / `orbitgui` / `orbit-gui`). Widgets code lives in target `orbitgui`.
- No QSS / stylesheet. Dimmed placeholder text uses `QPalette::PlaceholderText`.
- Public contract preserved: `PreferencesDialog(const AppSettings&, QWidget* = nullptr)` and `AppSettings result() const` keep the same semantics.
- Existing test hooks preserved verbatim: `setConcurrentForTest`, `setMaxKBpsForTest`, `setUserAgentCustomForTest`, `setBrowserEnabledForTest`, `setStartAtLoginForTest`.
- Do not modify `Settings.h/.cpp`, `SettingsIo`, JSON format, or defaults.
- Existing tests in `tests/tst_gui.cpp` must keep passing.
- Commit messages: Conventional Commits, in English, no co-authorship. Do NOT commit without explicit user authorization — the final "commit" steps below are drafted but must only run once the user authorizes them.
- Build/run happens via CLion (`cmake-build-debug`); command-line build steps below assume a configured build dir.

## File Structure

- `src/gui/PreferencesDialog.h` — add `Category` enum, `setInitialCategory`, Scheduler widget members, navigation members (`m_categoryList`, `m_stack`), Scheduler test hooks, and a private `loadFromSettings` helper declaration.
- `src/gui/PreferencesDialog.cpp` — full rewrite of the constructor into list+stack pages, section-header/placeholder helpers, `loadFromSettings`, extended `result()`, Restore-defaults wiring, Scheduler test hooks.
- `src/gui/SchedulerDialog.{h,cpp}` — deleted.
- `src/gui/CMakeLists.txt` — drop `SchedulerDialog.cpp`.
- `src/gui/MainWindow.cpp` — `onScheduler()` reopens Preferences on the Scheduler category; `applyPreferencesResult()` re-arms the scheduler; remove `SchedulerDialog` include/usage.
- `tests/tst_gui.cpp` — add category-list, scheduler-via-Preferences, `setInitialCategory`, and restore-defaults tests; remove the old `SchedulerDialog` test.

## Category order (stable index for `Category` enum)

`General(0)`, `Location(1)`, `DownloadsConnection(2)`, `Limits(3)`, `Appearance(4)`, `Monitoring(5)`, `Scheduler(6)`, `P2PNetwork(7)`, `Proxy(8)`, `Others(9)`.

The `QListWidget` rows and `QStackedWidget` pages are inserted in exactly this order, so `int(Category)` equals both the row and the page index.

---

### Task 1: Rewrite PreferencesDialog into the sidebar layout

Delivers the full new dialog: list+stack navigation, all real categories reorganized, the Scheduler section, the placeholder pages, the `Category` enum, `loadFromSettings`, and an extended `result()` that round-trips `scheduler`. OK/Cancel footer only (Restore defaults arrives in Task 3). All existing tests keep passing; two new tests are added.

**Files:**
- Modify: `src/gui/PreferencesDialog.h`
- Modify: `src/gui/PreferencesDialog.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `AppSettings`, `SchedulerConfig`, `Recurrence { Once, Daily }`, `ThemePref`, `ClipboardMode`, `generateBridgeToken()`.
- Produces:
  - `enum class PreferencesDialog::Category { General, Location, DownloadsConnection, Limits, Appearance, Monitoring, Scheduler, P2PNetwork, Proxy, Others };`
  - `AppSettings PreferencesDialog::result() const` — now also fills `.scheduler`.
  - Scheduler test hooks: `void setSchedulerEnabledForTest(bool)`, `void setSchedulerStartForTest(QTime)`, `void setSchedulerStopForTest(QTime)`, `void setSchedulerRecurrenceForTest(Recurrence)`, `void setSchedulerQuitForTest(bool)`.
  - Navigation members `QListWidget* m_categoryList` and `QStackedWidget* m_stack` (used by Task 2).

- [ ] **Step 1: Write the failing tests**

Add to `tests/tst_gui.cpp` (in the same test class, after `preferencesStartAtLoginRoundTrips`):

```cpp
    // --- Preferences sidebar redesign --------------------------------------

    void preferences_has_ten_category_rows_synced_to_stack() {
        AppSettings in;
        PreferencesDialog dlg(in);
        auto* list = dlg.findChild<QListWidget*>();
        auto* stack = dlg.findChild<QStackedWidget*>();
        QVERIFY(list);
        QVERIFY(stack);
        QCOMPARE(list->count(), 10);
        QCOMPARE(stack->count(), 10);
        list->setCurrentRow(6);                 // Scheduler
        QCOMPARE(stack->currentIndex(), 6);
    }

    void preferences_result_reflects_scheduler_widgets() {
        AppSettings in;                          // scheduler defaults: disabled, 08:00-18:00, Daily
        PreferencesDialog dlg(in);
        dlg.setSchedulerEnabledForTest(true);
        dlg.setSchedulerStartForTest(QTime(9, 15));
        dlg.setSchedulerStopForTest(QTime(23, 45));
        dlg.setSchedulerRecurrenceForTest(Recurrence::Once);
        dlg.setSchedulerQuitForTest(true);
        const AppSettings out = dlg.result();
        QVERIFY(out.scheduler.enabled);
        QCOMPARE(out.scheduler.start, QTime(9, 15));
        QCOMPARE(out.scheduler.stop, QTime(23, 45));
        QVERIFY(out.scheduler.recurrence == Recurrence::Once);
        QVERIFY(out.scheduler.quitWhenDone);
    }
```

Add the includes near the top of `tests/tst_gui.cpp` if not already present:

```cpp
#include <QListWidget>
#include <QStackedWidget>
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: compile failure (`setSchedulerEnabledForTest` undeclared / no `QListWidget` child), i.e. the new tests do not build/pass yet.

- [ ] **Step 3: Rewrite the header**

Replace the entire contents of `src/gui/PreferencesDialog.h` with:

```cpp
#pragma once
#include "Settings.h"
#include <QDialog>
#include <QTime>
class QSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTimeEdit;
class QListWidget;
class QStackedWidget;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    enum class Category {
        General, Location, DownloadsConnection, Limits, Appearance,
        Monitoring, Scheduler, P2PNetwork, Proxy, Others
    };

    explicit PreferencesDialog(const AppSettings& current, QWidget* parent = nullptr);
    AppSettings result() const;
    void setInitialCategory(Category c);

    // Test hooks (no exec() needed):
    void setConcurrentForTest(int n);
    void setMaxKBpsForTest(int kbps);
    void setUserAgentCustomForTest(const QString& ua);
    void setBrowserEnabledForTest(bool on);
    void setStartAtLoginForTest(bool on);
    void setSchedulerEnabledForTest(bool on);
    void setSchedulerStartForTest(QTime t);
    void setSchedulerStopForTest(QTime t);
    void setSchedulerRecurrenceForTest(Recurrence r);
    void setSchedulerQuitForTest(bool on);

private:
    void loadFromSettings(const AppSettings& s);   // (re)apply values onto existing widgets

    AppSettings m_base;                 // preserves non-edited fields
    // Navigation
    QListWidget*    m_categoryList = nullptr;
    QStackedWidget* m_stack        = nullptr;
    // Appearance
    QComboBox* m_theme      = nullptr;
    // General / Location / Downloads / Limits
    QSpinBox*  m_concurrent = nullptr;
    QSpinBox*  m_segments   = nullptr;
    QSpinBox*  m_maxKBps    = nullptr;  // 0 = unlimited
    QLineEdit* m_dir        = nullptr;
    QComboBox* m_clipMode   = nullptr;
    QComboBox* m_uaPreset   = nullptr;
    QLineEdit* m_uaCustom   = nullptr;
    QCheckBox* m_startAtLogin = nullptr;
    QSpinBox*  m_connectMs  = nullptr;
    QSpinBox*  m_idleMs     = nullptr;
    QSpinBox*  m_retries    = nullptr;
    QSpinBox*  m_backoffMs  = nullptr;
    QSpinBox*  m_minSegKB   = nullptr;
    QSpinBox*  m_throttleMs = nullptr;
    // Monitoring (browser bridge)
    QCheckBox* m_brEnabled  = nullptr;
    QSpinBox*  m_brPort     = nullptr;
    QLineEdit* m_brToken    = nullptr;   // read-only
    QString    m_brTokenValue;
    // Scheduler
    QCheckBox* m_schedEnabled    = nullptr;
    QTimeEdit* m_schedStart      = nullptr;
    QTimeEdit* m_schedStop       = nullptr;
    QComboBox* m_schedRecurrence = nullptr;
    QCheckBox* m_schedQuit       = nullptr;
};
```

- [ ] **Step 4: Rewrite the source**

Replace the entire contents of `src/gui/PreferencesDialog.cpp` with:

```cpp
#include "PreferencesDialog.h"
#include "BrowserBridgeProtocol.h"
#include <QListWidget>
#include <QStackedWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QTimeEdit>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QFileDialog>
#include <QApplication>
#include <QClipboard>

static const char* kChromeUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
static const char* kCurlUA = "curl/8.7.1";

// Wraps a category body in a page with a bold header + separator line.
static QWidget* makeSectionPage(const QString& title, QLayout* body, QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* v = new QVBoxLayout(page);
    auto* header = new QLabel(title, page);
    QFont hf = header->font(); hf.setBold(true); header->setFont(hf);
    auto* line = new QFrame(page);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    v->addWidget(header);
    v->addWidget(line);
    v->addLayout(body);
    v->addStretch();
    return page;
}

// A "coming soon" placeholder page (centered, dimmed text, no QSS).
static QWidget* makePlaceholderPage(const QString& title, QWidget* parent) {
    auto* lbl = new QLabel(QObject::tr("This section is coming soon."), parent);
    lbl->setAlignment(Qt::AlignCenter);
    QPalette pal = lbl->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    lbl->setPalette(pal);
    auto* body = new QVBoxLayout;
    body->addStretch();
    body->addWidget(lbl);
    body->addStretch();
    return makeSectionPage(title, body, parent);
}

PreferencesDialog::PreferencesDialog(const AppSettings& current, QWidget* parent)
    : QDialog(parent), m_base(current) {
    setWindowTitle(tr("Preferences"));

    m_categoryList = new QListWidget(this);
    m_categoryList->setFixedWidth(160);
    m_stack = new QStackedWidget(this);

    // ---- General (start-at-login + clipboard) ----
    {
        m_clipMode = new QComboBox;
        m_clipMode->addItem("Off",    int(ClipboardMode::Off));
        m_clipMode->addItem("Ask",    int(ClipboardMode::Ask));
        m_clipMode->addItem("Auto",   int(ClipboardMode::Auto));
        m_clipMode->addItem("Notify", int(ClipboardMode::Notify));
        m_startAtLogin = new QCheckBox(tr("Start Orbit at login"));
        auto* f = new QFormLayout;
        f->addRow(QString(), m_startAtLogin);
        f->addRow(tr("Clipboard monitor:"), m_clipMode);
        m_categoryList->addItem(tr("General"));
        m_stack->addWidget(makeSectionPage(tr("General"), f, this));
    }

    // ---- Location (default download folder) ----
    {
        auto* dirRow = new QWidget;
        auto* dl = new QHBoxLayout(dirRow);
        dl->setContentsMargins(0, 0, 0, 0);
        m_dir = new QLineEdit(dirRow);
        auto* browse = new QPushButton(tr("Browse…"), dirRow);
        dl->addWidget(m_dir);
        dl->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(
                this, tr("Default download folder"), m_dir->text());
            if (!d.isEmpty()) m_dir->setText(d);
        });
        auto* f = new QFormLayout;
        f->addRow(tr("Default folder:"), dirRow);
        m_categoryList->addItem(tr("Location"));
        m_stack->addWidget(makeSectionPage(tr("Location"), f, this));
    }

    // ---- Downloads/Connection ----
    {
        m_concurrent = new QSpinBox; m_concurrent->setRange(1, 32);
        m_segments   = new QSpinBox; m_segments->setRange(1, 32);
        m_uaPreset = new QComboBox;
        m_uaPreset->addItem(tr("curl (curl/8.7.1)"), kCurlUA);
        m_uaPreset->addItem(tr("Chrome"),            kChromeUA);
        m_uaPreset->addItem(tr("Custom…"),           QString());
        m_uaCustom = new QLineEdit;
        auto syncUa = [this] {
            m_uaCustom->setEnabled(m_uaPreset->currentData().toString().isEmpty());
        };
        connect(m_uaPreset, &QComboBox::currentIndexChanged, this, [syncUa](int) { syncUa(); });
        m_connectMs  = new QSpinBox; m_connectMs->setRange(1000, 600000);
        m_idleMs     = new QSpinBox; m_idleMs->setRange(1000, 600000);
        m_retries    = new QSpinBox; m_retries->setRange(0, 100);
        m_backoffMs  = new QSpinBox; m_backoffMs->setRange(0, 60000);
        m_minSegKB   = new QSpinBox; m_minSegKB->setRange(1, 1'000'000);
        m_throttleMs = new QSpinBox; m_throttleMs->setRange(0, 5000);
        auto* f = new QFormLayout;
        f->addRow(tr("Simultaneous downloads:"), m_concurrent);
        f->addRow(tr("Segments per download:"),  m_segments);
        f->addRow(tr("User-Agent:"),             m_uaPreset);
        f->addRow(QString(),                     m_uaCustom);
        f->addRow(tr("Connect timeout (ms):"),   m_connectMs);
        f->addRow(tr("Idle timeout (ms):"),      m_idleMs);
        f->addRow(tr("Max segment retries:"),    m_retries);
        f->addRow(tr("Retry backoff base (ms):"), m_backoffMs);
        f->addRow(tr("Min segment size (KB):"),  m_minSegKB);
        f->addRow(tr("Progress throttle (ms):"), m_throttleMs);
        m_categoryList->addItem(tr("Downloads/Connection"));
        m_stack->addWidget(makeSectionPage(tr("Downloads/Connection"), f, this));
    }

    // ---- Limits ----
    {
        m_maxKBps = new QSpinBox; m_maxKBps->setRange(0, 1'000'000);
        m_maxKBps->setSuffix(" KB/s");
        m_maxKBps->setSpecialValueText(tr("Unlimited"));
        auto* f = new QFormLayout;
        f->addRow(tr("Max speed:"), m_maxKBps);
        m_categoryList->addItem(tr("Limits"));
        m_stack->addWidget(makeSectionPage(tr("Limits"), f, this));
    }

    // ---- Appearance ----
    {
        m_theme = new QComboBox;
        m_theme->addItem(tr("System"), int(ThemePref::System));
        m_theme->addItem(tr("Light"),  int(ThemePref::Light));
        m_theme->addItem(tr("Dark"),   int(ThemePref::Dark));
        auto* f = new QFormLayout;
        f->addRow(tr("Theme:"), m_theme);
        m_categoryList->addItem(tr("Appearance"));
        m_stack->addWidget(makeSectionPage(tr("Appearance"), f, this));
    }

    // ---- Monitoring (browser bridge) ----
    {
        m_brEnabled = new QCheckBox(tr("Enable browser bridge"));
        m_brPort = new QSpinBox; m_brPort->setRange(1, 65535);
        m_brToken = new QLineEdit; m_brToken->setReadOnly(true);
        auto* tokenRow = new QWidget;
        auto* btl = new QHBoxLayout(tokenRow);
        btl->setContentsMargins(0, 0, 0, 0);
        auto* brCopy  = new QPushButton(tr("Copy"), tokenRow);
        auto* brRegen = new QPushButton(tr("Regenerate"), tokenRow);
        btl->addWidget(m_brToken);
        btl->addWidget(brCopy);
        btl->addWidget(brRegen);
        connect(m_brEnabled, &QCheckBox::toggled, this, [this](bool on) {
            if (on && m_brTokenValue.isEmpty()) {
                m_brTokenValue = generateBridgeToken();
                m_brToken->setText(m_brTokenValue);
            }
        });
        connect(brRegen, &QPushButton::clicked, this, [this] {
            m_brTokenValue = generateBridgeToken();
            m_brToken->setText(m_brTokenValue);
        });
        connect(brCopy, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(m_brTokenValue);
        });
        auto* hint = new QLabel(
            tr("The app listens on 127.0.0.1:<port>. Paste this token into the "
               "browser extension's options."));
        hint->setWordWrap(true);
        auto* f = new QFormLayout;
        f->addRow(QString(), m_brEnabled);
        f->addRow(tr("Port:"), m_brPort);
        f->addRow(tr("Token:"), tokenRow);
        f->addRow(QString(), hint);
        m_categoryList->addItem(tr("Monitoring"));
        m_stack->addWidget(makeSectionPage(tr("Monitoring"), f, this));
    }

    // ---- Scheduler ----
    {
        m_schedEnabled = new QCheckBox(tr("Enable schedule"));
        m_schedStart = new QTimeEdit; m_schedStart->setDisplayFormat("HH:mm");
        m_schedStop  = new QTimeEdit; m_schedStop->setDisplayFormat("HH:mm");
        m_schedRecurrence = new QComboBox;
        m_schedRecurrence->addItem(tr("Daily"), int(Recurrence::Daily));
        m_schedRecurrence->addItem(tr("Once"),  int(Recurrence::Once));
        m_schedQuit = new QCheckBox(tr("Quit when all downloads finish"));
        auto* f = new QFormLayout;
        f->addRow(QString(), m_schedEnabled);
        f->addRow(tr("Start:"), m_schedStart);
        f->addRow(tr("Stop:"),  m_schedStop);
        f->addRow(tr("Recurrence:"), m_schedRecurrence);
        f->addRow(QString(), m_schedQuit);
        m_categoryList->addItem(tr("Scheduler"));
        m_stack->addWidget(makeSectionPage(tr("Scheduler"), f, this));
    }

    // ---- Placeholders ----
    m_categoryList->addItem(tr("P2P Network"));
    m_stack->addWidget(makePlaceholderPage(tr("P2P Network"), this));
    m_categoryList->addItem(tr("Proxy"));
    m_stack->addWidget(makePlaceholderPage(tr("Proxy"), this));
    m_categoryList->addItem(tr("Others"));
    m_stack->addWidget(makePlaceholderPage(tr("Others"), this));

    connect(m_categoryList, &QListWidget::currentRowChanged,
            m_stack, &QStackedWidget::setCurrentIndex);

    loadFromSettings(current);
    m_categoryList->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* body = new QHBoxLayout;
    body->addWidget(m_categoryList);
    body->addWidget(m_stack, 1);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addWidget(buttons);
}

void PreferencesDialog::loadFromSettings(const AppSettings& s) {
    m_theme->setCurrentIndex(m_theme->findData(int(s.ui.theme)));
    m_concurrent->setValue(s.engine.maxConcurrentDownloads);
    m_segments->setValue(s.engine.segmentCount);
    m_maxKBps->setValue(int(s.engine.maxBytesPerSec / 1024));
    m_dir->setText(s.ui.defaultDownloadDir);
    m_clipMode->setCurrentIndex(m_clipMode->findData(int(s.ui.clipboardMode)));
    if (s.engine.userAgent == kCurlUA) {
        m_uaPreset->setCurrentIndex(0); m_uaCustom->clear();
    } else if (s.engine.userAgent == kChromeUA) {
        m_uaPreset->setCurrentIndex(1); m_uaCustom->clear();
    } else {
        m_uaPreset->setCurrentIndex(2); m_uaCustom->setText(s.engine.userAgent);
    }
    m_uaCustom->setEnabled(m_uaPreset->currentData().toString().isEmpty());
    m_startAtLogin->setChecked(s.ui.startAtLogin);
    m_connectMs->setValue(s.engine.connectTimeoutMs);
    m_idleMs->setValue(s.engine.idleTimeoutMs);
    m_retries->setValue(s.engine.maxSegmentRetries);
    m_backoffMs->setValue(s.engine.retryBackoffBaseMs);
    m_minSegKB->setValue(int(s.engine.minSegSize / 1024));
    m_throttleMs->setValue(s.engine.progressThrottleMs);
    m_brEnabled->setChecked(s.browser.enabled);
    m_brPort->setValue(s.browser.port);
    m_brTokenValue = s.browser.token;
    m_brToken->setText(m_brTokenValue);
    m_schedEnabled->setChecked(s.scheduler.enabled);
    m_schedStart->setTime(s.scheduler.start);
    m_schedStop->setTime(s.scheduler.stop);
    m_schedRecurrence->setCurrentIndex(m_schedRecurrence->findData(int(s.scheduler.recurrence)));
    m_schedQuit->setChecked(s.scheduler.quitWhenDone);
}

AppSettings PreferencesDialog::result() const {
    AppSettings s = m_base;   // keep non-editable fields
    s.engine.maxConcurrentDownloads = m_concurrent->value();
    s.engine.segmentCount           = m_segments->value();
    s.engine.maxBytesPerSec         = qint64(m_maxKBps->value()) * 1024;
    s.engine.connectTimeoutMs       = m_connectMs->value();
    s.engine.idleTimeoutMs          = m_idleMs->value();
    s.engine.maxSegmentRetries      = m_retries->value();
    s.engine.retryBackoffBaseMs     = m_backoffMs->value();
    s.engine.minSegSize             = qint64(m_minSegKB->value()) * 1024;
    s.engine.progressThrottleMs     = m_throttleMs->value();
    const QString preset = m_uaPreset->currentData().toString();
    s.engine.userAgent = preset.isEmpty() ? m_uaCustom->text() : preset;
    s.ui.defaultDownloadDir = m_dir->text();
    s.ui.clipboardMode = ClipboardMode(m_clipMode->currentData().toInt());
    s.ui.theme = ThemePref(m_theme->currentData().toInt());
    s.ui.startAtLogin = m_startAtLogin->isChecked();
    s.browser.enabled = m_brEnabled->isChecked();
    s.browser.port    = quint16(m_brPort->value());
    s.browser.token   = m_brTokenValue;
    s.scheduler.enabled      = m_schedEnabled->isChecked();
    s.scheduler.start        = m_schedStart->time();
    s.scheduler.stop         = m_schedStop->time();
    s.scheduler.recurrence   = Recurrence(m_schedRecurrence->currentData().toInt());
    s.scheduler.quitWhenDone = m_schedQuit->isChecked();
    return s;
}

void PreferencesDialog::setInitialCategory(Category c) {
    int row = int(c);
    if (row < 0 || row >= m_categoryList->count()) row = 0;
    m_categoryList->setCurrentRow(row);
}

void PreferencesDialog::setConcurrentForTest(int n)    { m_concurrent->setValue(n); }
void PreferencesDialog::setMaxKBpsForTest(int kbps)   { m_maxKBps->setValue(kbps); }
void PreferencesDialog::setUserAgentCustomForTest(const QString& ua) {
    m_uaPreset->setCurrentIndex(2);   // Custom…
    m_uaCustom->setText(ua);
}
void PreferencesDialog::setBrowserEnabledForTest(bool on) {
    m_brEnabled->setChecked(on);
    if (on && m_brTokenValue.isEmpty()) {
        m_brTokenValue = generateBridgeToken();
        m_brToken->setText(m_brTokenValue);
    }
}
void PreferencesDialog::setStartAtLoginForTest(bool on) { m_startAtLogin->setChecked(on); }

void PreferencesDialog::setSchedulerEnabledForTest(bool on) { m_schedEnabled->setChecked(on); }
void PreferencesDialog::setSchedulerStartForTest(QTime t)   { m_schedStart->setTime(t); }
void PreferencesDialog::setSchedulerStopForTest(QTime t)    { m_schedStop->setTime(t); }
void PreferencesDialog::setSchedulerRecurrenceForTest(Recurrence r) {
    m_schedRecurrence->setCurrentIndex(m_schedRecurrence->findData(int(r)));
}
void PreferencesDialog::setSchedulerQuitForTest(bool on)    { m_schedQuit->setChecked(on); }
```

Note: `setBrowserEnabledForTest` sets `m_brEnabled->setChecked(on)`. Because `loadFromSettings` runs during construction and the `toggled` connect is already in place, this preserves the existing "enabling generates a token" behavior (the explicit block is a safety net for the case where `setChecked` does not emit).

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: PASS — including the two new tests and all pre-existing `preferences_*` tests. The old `scheduler_dialog_result_reflects_widgets` (still using `SchedulerDialog`) also still passes.

- [ ] **Step 6: Commit** (only after user authorization)

```bash
git add src/gui/PreferencesDialog.h src/gui/PreferencesDialog.cpp tests/tst_gui.cpp
git commit -m "refactor(gui): rewrite Preferences into sidebar-of-categories layout"
```

---

### Task 2: `setInitialCategory` selection test

Locks the behavior that `MainWindow` relies on to open Preferences on the Scheduler page.

**Files:**
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `PreferencesDialog::setInitialCategory(Category)`, `m_stack` / `m_categoryList` (via `findChild`).

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp` after `preferences_result_reflects_scheduler_widgets`:

```cpp
    void preferences_setInitialCategory_selects_page() {
        AppSettings in;
        PreferencesDialog dlg(in);
        dlg.setInitialCategory(PreferencesDialog::Category::Scheduler);
        auto* stack = dlg.findChild<QStackedWidget*>();
        QVERIFY(stack);
        QCOMPARE(stack->currentIndex(), 6);   // Scheduler == index 6
    }
```

- [ ] **Step 2: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: PASS (implementation already exists from Task 1; this test documents/locks it).

- [ ] **Step 3: Commit** (only after user authorization)

```bash
git add tests/tst_gui.cpp
git commit -m "test(gui): cover PreferencesDialog::setInitialCategory selection"
```

---

### Task 3: Restore defaults button

Adds a footer "Restore defaults" button that repopulates every widget from a default `AppSettings{}` without closing the dialog or persisting.

**Files:**
- Modify: `src/gui/PreferencesDialog.cpp` (constructor footer only)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `loadFromSettings(const AppSettings&)` (from Task 1), `QDialogButtonBox`.
- Produces: a `QDialogButtonBox::ResetRole` button labeled "Restore defaults" wired to reset widget values.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp`:

```cpp
    void preferences_restore_defaults_resets_widgets() {
        AppSettings in;
        in.engine.maxConcurrentDownloads = 9;
        PreferencesDialog dlg(in);
        dlg.setConcurrentForTest(3);                 // diverge from defaults
        auto* box = dlg.findChild<QDialogButtonBox*>();
        QVERIFY(box);
        QAbstractButton* reset = nullptr;
        for (QAbstractButton* b : box->buttons())
            if (box->buttonRole(b) == QDialogButtonBox::ResetRole) { reset = b; break; }
        QVERIFY(reset);
        reset->click();
        // AppSettings{} default for maxConcurrentDownloads
        QCOMPARE(dlg.result().engine.maxConcurrentDownloads,
                 AppSettings{}.engine.maxConcurrentDownloads);
    }
```

Ensure `#include <QDialogButtonBox>` and `#include <QAbstractButton>` are present in `tests/tst_gui.cpp` (add if missing).

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: FAIL — no button with `ResetRole` exists yet (`reset` is null → `QVERIFY(reset)` fails).

- [ ] **Step 3: Add the Restore defaults button**

In `src/gui/PreferencesDialog.cpp`, replace the button-box creation block in the constructor:

```cpp
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
```

with:

```cpp
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* restore = buttons->addButton(tr("Restore defaults"),
                                       QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(restore, &QPushButton::clicked, this, [this] {
        loadFromSettings(AppSettings{});
    });
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: PASS.

- [ ] **Step 5: Commit** (only after user authorization)

```bash
git add src/gui/PreferencesDialog.cpp tests/tst_gui.cpp
git commit -m "feat(gui): add Restore defaults to Preferences"
```

---

### Task 4: Route Scheduler action through Preferences and re-arm the scheduler

The toolbar/menu "Scheduler" action now opens Preferences pre-selected on the Scheduler category, and `applyPreferencesResult` re-arms the runtime scheduler so schedule edits made via Preferences take effect immediately.

**Files:**
- Modify: `src/gui/MainWindow.cpp` (`onScheduler`, `applyPreferencesResult`)

**Interfaces:**
- Consumes: `PreferencesDialog::setInitialCategory(Category::Scheduler)`, `applySchedulerConfig(const SchedulerConfig&)`, `applyPreferencesResult(const AppSettings&)`.

- [ ] **Step 1: Rewrite `onScheduler`**

In `src/gui/MainWindow.cpp`, replace:

```cpp
void MainWindow::onScheduler() {
    SchedulerDialog dlg(m_settings.scheduler, this);
    if (dlg.exec() != QDialog::Accepted) return;
    applySchedulerConfig(dlg.result());
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}
```

with:

```cpp
void MainWindow::onScheduler() {
    PreferencesDialog dlg(m_settings, this);
    dlg.setInitialCategory(PreferencesDialog::Category::Scheduler);
    if (dlg.exec() != QDialog::Accepted) return;
    applyPreferencesResult(dlg.result());
}
```

- [ ] **Step 2: Re-arm the scheduler in `applyPreferencesResult`**

In `src/gui/MainWindow.cpp`, in `applyPreferencesResult`, add a scheduler re-arm right after `applyBrowserBridge(m_settings.browser);`:

```cpp
    applyBrowserBridge(m_settings.browser);
    applySchedulerConfig(m_settings.scheduler);   // re-arm timer/state on schedule changes
```

(`applySchedulerConfig` sets `m_settings.scheduler = sc`, which is already equal here, then calls `m_scheduler.setConfig(...)` and (re)starts the 30s tick timer.)

- [ ] **Step 3: Remove the `SchedulerDialog` include**

In `src/gui/MainWindow.cpp`, delete the line:

```cpp
#include "SchedulerDialog.h"
```

(Keep `#include "PreferencesDialog.h"`.)

- [ ] **Step 4: Build and run the full GUI test suite**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: PASS. `MainWindow` compiles without `SchedulerDialog`. The scheduler-boot tests using `applySchedulerConfigForTest` still pass (that path is unchanged).

- [ ] **Step 5: Commit** (only after user authorization)

```bash
git add src/gui/MainWindow.cpp
git commit -m "feat(gui): open Scheduler inside Preferences and re-arm on apply"
```

---

### Task 5: Delete SchedulerDialog and migrate its test

Removes the now-unused `SchedulerDialog` class and its dedicated test (its behavior is covered by `preferences_result_reflects_scheduler_widgets` from Task 1).

**Files:**
- Delete: `src/gui/SchedulerDialog.h`, `src/gui/SchedulerDialog.cpp`
- Modify: `src/gui/CMakeLists.txt`
- Modify: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: nothing new. This task only removes dead code.

- [ ] **Step 1: Remove the old SchedulerDialog test**

In `tests/tst_gui.cpp`, delete the `#include "SchedulerDialog.h"` line and the entire `scheduler_dialog_result_reflects_widgets()` test method (the block starting at the `// --- Task 14 (Fase 4): SchedulerDialog` comment through its closing brace).

- [ ] **Step 2: Delete the source files**

Run:

```bash
git rm src/gui/SchedulerDialog.h src/gui/SchedulerDialog.cpp
```

- [ ] **Step 3: Remove from CMake**

In `src/gui/CMakeLists.txt`, delete the line:

```cmake
    SchedulerDialog.cpp
```

- [ ] **Step 4: Build and run the full test suite**

Run: `cmake --build cmake-build-debug --target tst_gui && ./cmake-build-debug/tests/tst_gui`
Expected: PASS. No references to `SchedulerDialog` remain (`grep -rn SchedulerDialog src tests` returns nothing).

- [ ] **Step 5: Manual smoke check (GUI)**

Build and launch the app via CLion (`cmake-build-debug`). Open Preferences: verify the left category list shows the 10 categories, switching rows swaps the right panel with a bold section header, the Scheduler category shows the schedule controls, placeholder pages show the dimmed "coming soon" text, and the Scheduler toolbar/menu action opens Preferences already on the Scheduler page. Confirm OK persists and Restore defaults repopulates the fields.

- [ ] **Step 6: Commit** (only after user authorization)

```bash
git add -A
git commit -m "refactor(gui): remove standalone SchedulerDialog"
```

---

## Self-Review

**Spec coverage:**
- Sidebar layout (list+stack) + section headers → Task 1. ✓
- Orbit-style category reorganization → Task 1 category map. ✓
- Restore defaults / OK / Cancel footer → Task 3 (Restore), Task 1 (OK/Cancel). ✓
- Scheduler moved into Preferences → Task 1 (widgets/result), Task 4 (action routing + re-arm). ✓
- Remove standalone SchedulerDialog → Task 5. ✓
- Placeholder "coming soon" categories → Task 1 (`makePlaceholderPage`). ✓
- Native styling via QPalette, no QSS → `makeSectionPage`/`makePlaceholderPage` use `QFont`/`QPalette` only. ✓
- Public contract + test hooks preserved → Task 1 header keeps all five existing hooks and `result()` semantics. ✓
- `setInitialCategory` → Task 1 (impl) + Task 2 (test). ✓
- Persistence untouched → no task modifies `Settings.*`/`SettingsIo`. ✓

**Placeholder scan:** No "TBD"/"handle edge cases"/"similar to" — every code step shows full code. ✓

**Type consistency:** `Category` enum order matches insertion order and the index asserts (`Scheduler == 6`). `Recurrence(m_schedRecurrence->currentData().toInt())` matches `enum class Recurrence { Once, Daily }`. `loadFromSettings` sets every widget `result()` reads. `applySchedulerConfig` signature matches its use in Task 4. ✓

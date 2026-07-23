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
    auto* restore = buttons->addButton(tr("Restore defaults"),
                                       QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(restore, &QPushButton::clicked, this, [this] {
        loadFromSettings(AppSettings{});
    });

    auto* note = new QLabel(
        tr("Speed and simultaneous-download limits apply immediately; other changes "
           "apply to new downloads."), this);
    note->setWordWrap(true);

    auto* body = new QHBoxLayout;
    body->addWidget(m_categoryList);
    body->addWidget(m_stack, 1);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addWidget(note);
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

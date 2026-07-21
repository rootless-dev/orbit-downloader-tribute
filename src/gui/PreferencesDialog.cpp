#include "PreferencesDialog.h"
#include "BrowserBridgeProtocol.h"
#include <QTabWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QApplication>
#include <QClipboard>

static const char* kChromeUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
static const char* kCurlUA = "curl/8.7.1";

PreferencesDialog::PreferencesDialog(const AppSettings& current, QWidget* parent)
    : QDialog(parent), m_base(current) {
    setWindowTitle(tr("Preferences"));
    auto* tabs = new QTabWidget(this);

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

    // ---- General ----
    auto* gen = new QWidget(this);
    auto* gf  = new QFormLayout(gen);
    m_concurrent = new QSpinBox(gen); m_concurrent->setRange(1, 32);
    m_concurrent->setValue(current.engine.maxConcurrentDownloads);
    m_segments = new QSpinBox(gen); m_segments->setRange(1, 32);
    m_segments->setValue(current.engine.segmentCount);
    m_maxKBps = new QSpinBox(gen); m_maxKBps->setRange(0, 1'000'000);
    m_maxKBps->setSuffix(" KB/s"); m_maxKBps->setSpecialValueText(tr("Unlimited"));
    m_maxKBps->setValue(int(current.engine.maxBytesPerSec / 1024));
    auto* dirRow = new QWidget(gen); auto* dl = new QHBoxLayout(dirRow);
    dl->setContentsMargins(0,0,0,0);
    m_dir = new QLineEdit(current.ui.defaultDownloadDir, dirRow);
    auto* browse = new QPushButton(tr("Browse…"), dirRow);
    dl->addWidget(m_dir); dl->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this]{
        const QString d = QFileDialog::getExistingDirectory(this, tr("Default download folder"), m_dir->text());
        if (!d.isEmpty()) m_dir->setText(d);
    });
    m_clipMode = new QComboBox(gen);
    m_clipMode->addItem("Off",    int(ClipboardMode::Off));
    m_clipMode->addItem("Ask",    int(ClipboardMode::Ask));
    m_clipMode->addItem("Auto",   int(ClipboardMode::Auto));
    m_clipMode->addItem("Notify", int(ClipboardMode::Notify));
    m_clipMode->setCurrentIndex(m_clipMode->findData(int(current.ui.clipboardMode)));
    m_uaPreset = new QComboBox(gen);
    m_uaPreset->addItem(tr("curl (curl/8.7.1)"), kCurlUA);
    m_uaPreset->addItem(tr("Chrome"),            kChromeUA);
    m_uaPreset->addItem(tr("Custom…"),           QString());
    m_uaCustom = new QLineEdit(gen);
    // seleciona preset conforme o valor atual; senão, Custom
    if (current.engine.userAgent == kCurlUA)        m_uaPreset->setCurrentIndex(0);
    else if (current.engine.userAgent == kChromeUA) m_uaPreset->setCurrentIndex(1);
    else { m_uaPreset->setCurrentIndex(2); m_uaCustom->setText(current.engine.userAgent); }
    auto syncUa = [this]{ m_uaCustom->setEnabled(m_uaPreset->currentData().toString().isEmpty()); };
    connect(m_uaPreset, &QComboBox::currentIndexChanged, this, [syncUa](int){ syncUa(); });
    syncUa();

    gf->addRow(tr("Simultaneous downloads:"), m_concurrent);
    gf->addRow(tr("Segments per download:"),  m_segments);
    gf->addRow(tr("Max speed:"),              m_maxKBps);
    gf->addRow(tr("Default folder:"),         dirRow);
    gf->addRow(tr("Clipboard monitor:"),      m_clipMode);
    gf->addRow(tr("User-Agent:"),             m_uaPreset);
    gf->addRow(QString(),                     m_uaCustom);
    m_startAtLogin = new QCheckBox(tr("Start Orbit at login"), gen);
    m_startAtLogin->setChecked(current.ui.startAtLogin);
    gf->addRow(QString(), m_startAtLogin);
    tabs->addTab(gen, tr("General"));

    // ---- Advanced ----
    auto* adv = new QWidget(this);
    auto* af  = new QFormLayout(adv);
    auto mkSpin = [adv](int lo, int hi, int val){ auto* s=new QSpinBox(adv); s->setRange(lo,hi); s->setValue(val); return s; };
    m_connectMs  = mkSpin(1000, 600000, current.engine.connectTimeoutMs);
    m_idleMs     = mkSpin(1000, 600000, current.engine.idleTimeoutMs);
    m_retries    = mkSpin(0, 100,       current.engine.maxSegmentRetries);
    m_backoffMs  = mkSpin(0, 60000,     current.engine.retryBackoffBaseMs);
    m_minSegKB   = mkSpin(1, 1'000'000, int(current.engine.minSegSize / 1024));
    m_throttleMs = mkSpin(0, 5000,      current.engine.progressThrottleMs);
    af->addRow(tr("Connect timeout (ms):"), m_connectMs);
    af->addRow(tr("Idle timeout (ms):"),    m_idleMs);
    af->addRow(tr("Max segment retries:"),  m_retries);
    af->addRow(tr("Retry backoff base (ms):"), m_backoffMs);
    af->addRow(tr("Min segment size (KB):"), m_minSegKB);
    af->addRow(tr("Progress throttle (ms):"), m_throttleMs);
    tabs->addTab(adv, tr("Advanced"));

    // ---- Browser ----
    auto* br  = new QWidget(this);
    auto* bf  = new QFormLayout(br);
    m_brTokenValue = current.browser.token;
    m_brEnabled = new QCheckBox(tr("Enable browser bridge"), br);
    m_brEnabled->setChecked(current.browser.enabled);
    m_brPort = new QSpinBox(br); m_brPort->setRange(1, 65535);
    m_brPort->setValue(current.browser.port);
    m_brToken = new QLineEdit(m_brTokenValue, br);
    m_brToken->setReadOnly(true);
    auto* brTokenRow = new QWidget(br); auto* btl = new QHBoxLayout(brTokenRow);
    btl->setContentsMargins(0,0,0,0);
    auto* brCopy = new QPushButton(tr("Copy"), brTokenRow);
    auto* brRegen = new QPushButton(tr("Regenerate"), brTokenRow);
    btl->addWidget(m_brToken); btl->addWidget(brCopy); btl->addWidget(brRegen);

    connect(m_brEnabled, &QCheckBox::toggled, this, [this](bool on){
        if (on && m_brTokenValue.isEmpty()) {
            m_brTokenValue = generateBridgeToken();
            m_brToken->setText(m_brTokenValue);
        }
    });
    connect(brRegen, &QPushButton::clicked, this, [this]{
        m_brTokenValue = generateBridgeToken();
        m_brToken->setText(m_brTokenValue);
    });
    connect(brCopy, &QPushButton::clicked, this, [this]{
        QApplication::clipboard()->setText(m_brTokenValue);
    });

    bf->addRow(QString(), m_brEnabled);
    bf->addRow(tr("Port:"), m_brPort);
    bf->addRow(tr("Token:"), brTokenRow);
    auto* brHint = new QLabel(
        tr("The app listens on 127.0.0.1:<port>. Paste this token into the "
           "browser extension's options."), br);
    brHint->setWordWrap(true);
    bf->addRow(QString(), brHint);
    tabs->addTab(br, tr("Browser"));

    auto* note = new QLabel(
        tr("Speed and simultaneous-download limits apply immediately; other changes "
           "apply to new downloads."), this);
    note->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(tabs);
    lay->addWidget(note);
    lay->addWidget(buttons);
}

AppSettings PreferencesDialog::result() const {
    AppSettings s = m_base;   // mantém o que não é editável aqui
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
    return s;
}

void PreferencesDialog::setConcurrentForTest(int n)              { m_concurrent->setValue(n); }
void PreferencesDialog::setMaxKBpsForTest(int kbps)             { m_maxKBps->setValue(kbps); }
void PreferencesDialog::setUserAgentCustomForTest(const QString& ua) {
    m_uaPreset->setCurrentIndex(2);   // Custom…
    m_uaCustom->setText(ua);
}

void PreferencesDialog::setBrowserEnabledForTest(bool on) {
    m_brEnabled->setChecked(on);
    if (on && m_brTokenValue.isEmpty()) {           // mesmo caminho do toggle real
        m_brTokenValue = generateBridgeToken();
        m_brToken->setText(m_brTokenValue);
    }
}

void PreferencesDialog::setStartAtLoginForTest(bool on) { m_startAtLogin->setChecked(on); }

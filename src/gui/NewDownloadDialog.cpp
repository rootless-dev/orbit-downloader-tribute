#include "NewDownloadDialog.h"
#include "DropTargets.h"
#include "FileType.h"
#include "UrlName.h"
#include "HttpProbe.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>

bool NewDownloadDialog::isValidDownloadUrl(const QUrl& u) {
    return isDownloadableScheme(u);
}

NewDownloadDialog::NewDownloadDialog(QWidget* parent, const QUrl& prefill) : QDialog(parent) {
    setWindowTitle("New Download");
    m_url  = new QLineEdit(this);
    m_url->setObjectName("urlEdit");
    m_dir  = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), this);
    m_dir->setObjectName("dirEdit");
    m_name = new QLineEdit(this);
    m_name->setObjectName("fileNameEdit");
    m_type = new QLabel("—", this);
    m_type->setObjectName("typeLabel");

    // Prefill explícito (drop/clipboard) tem prioridade; senão, tenta o
    // clipboard como a Fase 2 fazia.
    if (isValidDownloadUrl(prefill)) {
        m_url->setText(prefill.toString());
    } else {
        const QString clip = QApplication::clipboard()->text().trimmed();
        if (isValidDownloadUrl(QUrl(clip))) m_url->setText(clip);
    }

    auto* browse = new QPushButton("…", this);
    connect(browse, &QPushButton::clicked, this, &NewDownloadDialog::chooseDir);

    m_nam = new QNetworkAccessManager(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);
    connect(m_debounce, &QTimer::timeout, this, &NewDownloadDialog::startProbe);

    connect(m_url,  &QLineEdit::textChanged, this, &NewDownloadDialog::onUrlChanged);
    connect(m_name, &QLineEdit::textEdited,  this, [this]{ m_nameEdited = true; updateTypeLabel(); });

    // "Save to" row: directory line-edit + browse button side by side.
    auto* dirRow = new QWidget(this);
    auto* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    dirLay->addWidget(m_dir);
    dirLay->addWidget(browse);

    auto* form = new QFormLayout(this);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    setMinimumWidth(480);
    form->addRow("URL:", m_url);
    form->addRow("Save to:", dirRow);
    form->addRow("File:", m_name);
    form->addRow("Type:", m_type);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, [this]{
        if (isValidDownloadUrl(url())) accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(box);
    refreshName();
    if (isValidDownloadUrl(url())) m_debounce->start();   // prefill/clipboard também sonda
}

void NewDownloadDialog::chooseDir() {
    const QString d = QFileDialog::getExistingDirectory(this, "Save to", m_dir->text());
    if (!d.isEmpty()) m_dir->setText(d);
}

void NewDownloadDialog::refreshName() {
    m_name->setText(deriveFileName(url()));   // fallback visível imediato
    updateTypeLabel();
}

void NewDownloadDialog::updateTypeLabel() {
    m_type->setText(FileType::displayName(FileType::categorize(m_name->text())));
}

void NewDownloadDialog::onUrlChanged() {
    m_nameEdited = false;          // URL nova => nome sugerido é bem-vindo
    refreshName();                 // fallback visível imediato
    m_debounce->start();           // (re)agenda o probe
}

void NewDownloadDialog::startProbe() {
    const QUrl u = url();
    if (!isValidDownloadUrl(u) || u.scheme().compare("ftp", Qt::CaseInsensitive) == 0)
        return;                    // só HTTP(S)
    if (m_probe) { m_probe->deleteLater(); m_probe = nullptr; }
    m_probe = new HttpProbe(m_nam, EngineConfig{}.userAgent.toUtf8(), this);
    connect(m_probe, &HttpProbe::finished, this,
            [this, u](const ProbeResult& r){ applyProbeResult(u, r); });
    m_probe->start(u, Credentials{}, HeaderList{});
}

void NewDownloadDialog::applyProbeResult(const QUrl& probedUrl, const ProbeResult& res) {
    if (probedUrl != url()) return;               // probe obsoleto
    if (!res.ok || res.suggestedFileName.isEmpty()) return;
    if (m_nameEdited) return;                      // respeita edição manual
    m_name->setText(res.suggestedFileName);        // setText -> não dispara textEdited
    updateTypeLabel();
}

QUrl    NewDownloadDialog::url() const      { return QUrl(m_url->text().trimmed()); }
QString NewDownloadDialog::destPath() const { return QDir(m_dir->text()).filePath(m_name->text()); }

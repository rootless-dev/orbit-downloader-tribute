#pragma once
#include <QDialog>
#include <QUrl>
#include "DownloadTypes.h"          // ProbeResult
class QLineEdit;
class QLabel;
class QTimer;
class QNetworkAccessManager;
class HttpProbe;

class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    // prefill: URL vinda de um drop ou do clipboard (Tasks 12/13). Vazia = o
    // comportamento atual (tenta o clipboard).
    explicit NewDownloadDialog(QWidget* parent = nullptr, const QUrl& prefill = QUrl());
    QUrl        url() const;
    QString     destPath() const;
    static bool isValidDownloadUrl(const QUrl& u);   // http/https/ftp (spec §3.7)
    // Task 16: magnet: links aren't a downloadable URL (no host) and skip the
    // HTTP probe entirely (startProbe() already no-ops on them via
    // isValidDownloadUrl) - accepted as an alternate OK-button condition so a
    // pasted magnet: string doesn't get silently rejected by this dialog.
    static bool isValidMagnetUri(const QString& text);
    QString     magnetUri() const;   // trimmed URL-field text if isValidMagnetUri(), else empty
    QString     destDir() const;     // Task 16: dir alone, for the magnet path (no per-file name)
public slots:
    // Aplica o nome sugerido se `probedUrl` ainda é a URL atual, o nome não é
    // vazio e o usuário não editou o campo. Ponto de teste sem rede.
    void applyProbeResult(const QUrl& probedUrl, const ProbeResult& res);
private slots:
    void chooseDir();
    void refreshName();
    void onUrlChanged();
    void startProbe();
private:
    void updateTypeLabel();          // m_type a partir de m_name->text()
    QLineEdit* m_url;
    QLineEdit* m_dir;
    QLineEdit* m_name;               // era QLabel*
    QLabel*    m_type;
    QNetworkAccessManager* m_nam       = nullptr;
    QTimer*                m_debounce  = nullptr;
    HttpProbe*             m_probe     = nullptr;
    bool                   m_nameEdited = false;
};

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

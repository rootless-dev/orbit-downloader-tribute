#pragma once
#include "Transport.h"
#include <QByteArray>
class QNetworkAccessManager;
class QNetworkReply;

class HttpProbe : public Probe {
    Q_OBJECT
public:
    explicit HttpProbe(QNetworkAccessManager* nam, QByteArray userAgent, QObject* parent = nullptr);
    void start(const QUrl& url, const Credentials& creds,
              const HeaderList& extraHeaders) override;
private:
    void onMetaDataChanged();
    void onErrorOccurred();
    QNetworkAccessManager* m_nam;
    QByteArray             m_userAgent;
    QNetworkReply*         m_reply = nullptr;
    bool                   m_done  = false;
};

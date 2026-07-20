#pragma once
#include "BrowserBridgeProtocol.h"
#include "DownloadTypes.h"
#include <QObject>
#include <QHash>
class QTcpServer;
class QTcpSocket;

class BrowserBridge : public QObject {
    Q_OBJECT
public:
    explicit BrowserBridge(QObject* parent = nullptr);
    bool    start(quint16 port, const QString& token, const QString& allowedOrigin);
    void    stop();
    bool    listening() const;
    quint16 port() const;
signals:
    void downloadRequested(const QUrl& url, const HeaderList& headers,
                           const QString& suggestedFilename);
private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* sock);
    void sendJson(QTcpSocket* sock, int status, const char* reason, const QByteArray& body);
    void sendPreflight(QTcpSocket* sock);
    QByteArray corsBlock() const;

    QTcpServer* m_server = nullptr;
    QString     m_token, m_allowedOrigin;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    static constexpr int kMaxRequestBytes = 64 * 1024;
};

#pragma once
#include <QByteArray>
#include <QHttpServer>
#include <QTcpServer>
#include <QUrl>
#include <QHash>
#include <QSet>
#include <QStringList>

// In-process HTTP test double. Serves a deterministic body on several routes.
class TestServer {
public:
    explicit TestServer(QByteArray body);
    bool listen();                 // binds 127.0.0.1 on an ephemeral port
    quint16 port() const { return m_port; }
    QUrl url(const QString& path) const;      // e.g. url("/ranged")
    void setEtag(const QString& e) { m_etag = e; }
    // User-Agents distintos vistos na rota /ranged (probe + segmentos).
    QStringList userAgentsSeen() const { return QStringList(m_uaSeen.begin(), m_uaSeen.end()); }
    QStringList cookiesSeen()  const { return QStringList(m_cookiesSeen.begin(),  m_cookiesSeen.end()); }
    QStringList referersSeen() const { return QStringList(m_referersSeen.begin(), m_referersSeen.end()); }

private:
    QByteArray partial(qint64 start, qint64 end) const;   // inclusive
    QByteArray m_body;
    QString    m_etag = "\"v1\"";
    QHttpServer m_http;
    QTcpServer  m_tcp;
    quint16     m_port = 0;
    mutable QHash<QString,int> m_hits;        // per-path counter for /flaky
    QSet<QString> m_uaSeen;                    // User-Agents vistos em /ranged
    QSet<QString> m_cookiesSeen;
    QSet<QString> m_referersSeen;
    friend class TestServerRoutes;
};

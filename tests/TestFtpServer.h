#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QUrl>

class QTcpSocket;

// Servidor FTP mínimo, in-process, para testes offline. Fala só o subset que o
// cliente usa: USER, PASS, TYPE, SIZE, MDTM, PASV, REST, RETR, QUIT.
class TestFtpServer : public QObject {
    Q_OBJECT
public:
    explicit TestFtpServer(QByteArray content, QObject* parent = nullptr);

    bool    listen();
    quint16 port() const { return m_server.serverPort(); }
    QUrl    url(const QString& path = "/f.bin") const;

    void setContent(const QByteArray& c) { m_content = c; }
    void setMdtm(const QString& v)       { m_mdtm = v; }
    void setNoSize(bool on)              { m_noSize = on; }
    void setNoMdtm(bool on)              { m_noMdtm = on; }
    void setNoRest(bool on)              { m_noRest = on; }
    void requireAuth(const QString& u, const QString& p) { m_user = u; m_pass = p; }
    void setDropAfter(qint64 bytes)      { m_dropAfter = bytes; }
    void setDropOnce(bool on)            { m_dropOnce = on; }
    void setMissing(bool on)             { m_missing = on; }
    void setMaxConnections(int n)        { m_maxConn = n; }
    void setRestFailsAt(int nth)         { m_restFailsAt = nth; }
    int  controlConnections() const      { return m_connCount; }

private:
    struct Session;
    void onNewConnection();
    void onLine(Session* s, const QByteArray& line);
    void startTransfer(Session* s);

    QTcpServer m_server;
    QByteArray m_content;
    QString    m_mdtm = "20260717120000";
    bool       m_noSize = false, m_noMdtm = false, m_noRest = false;
    QString    m_user, m_pass;           // vazio = anônimo aceito
    qint64     m_dropAfter = -1;
    bool       m_dropOnce = false;
    bool       m_dropped  = false;       // já dropou uma vez? (p/ m_dropOnce)
    bool       m_missing  = false;
    int        m_maxConn = -1;
    int        m_restFailsAt = -1;
    int        m_connCount = 0;
};

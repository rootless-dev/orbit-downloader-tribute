#include "TestFtpServer.h"
#include <QTcpSocket>
#include <QTimer>
#include <QHostAddress>

struct TestFtpServer::Session {
    QTcpSocket* ctrl = nullptr;
    QByteArray  buf;
    QString     user;
    bool        loggedIn = false;
    qint64      rest = 0;
    QTcpServer* pasv = nullptr;     // listener passivo desta sessão
    QTcpSocket* data = nullptr;
    int         index = 0;          // n-ésima conexão de controle (1-based)
};

TestFtpServer::TestFtpServer(QByteArray content, QObject* parent)
    : QObject(parent), m_content(std::move(content)) {
    connect(&m_server, &QTcpServer::newConnection, this, &TestFtpServer::onNewConnection);
}

bool TestFtpServer::listen() {
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QUrl TestFtpServer::url(const QString& path) const {
    QUrl u;
    u.setScheme("ftp");
    u.setHost("127.0.0.1");
    u.setPort(port());
    u.setPath(path);
    return u;
}

static void send(QTcpSocket* s, const QString& line) {
    s->write((line + "\r\n").toUtf8());
    s->flush();
}

void TestFtpServer::onNewConnection() {
    while (QTcpSocket* c = m_server.nextPendingConnection()) {
        ++m_connCount;
        auto* s = new Session;
        s->ctrl  = c;
        s->index = m_connCount;

        if (m_maxConn >= 0 && m_connCount > m_maxConn) {
            send(c, "421 Too many connections");
            c->disconnectFromHost();
            delete s;
            continue;
        }

        connect(c, &QTcpSocket::readyRead, this, [this, s] {
            s->buf += s->ctrl->readAll();
            int eol;
            while ((eol = s->buf.indexOf("\r\n")) >= 0) {
                const QByteArray line = s->buf.left(eol);
                s->buf.remove(0, eol + 2);
                onLine(s, line);
            }
        });
        connect(c, &QTcpSocket::disconnected, this, [this, s] {
            // QUIT chama disconnectFromHost(), que emite `disconnected` de forma
            // SÍNCRONA, re-entrante, de dentro de onLine() — que por sua vez roda
            // dentro do while-loop do readyRead que ainda segura `s`. Deletar `s`
            // aqui liberaria o objeto sob o frame do readyRead, que volta a ler
            // s->buf ao retornar (use-after-free visto sob ASAN). Severa o
            // readyRead e adia o delete p/ a próxima volta do event loop, quando
            // o frame já desenrolou.
            s->ctrl->disconnect(this);
            if (s->pasv) s->pasv->deleteLater();
            if (s->data) s->data->deleteLater();
            s->ctrl->deleteLater();
            QTimer::singleShot(0, this, [s] { delete s; });
        });

        send(c, "220 TestFtpServer ready");
    }
}

void TestFtpServer::onLine(Session* s, const QByteArray& line) {
    const QString l   = QString::fromUtf8(line).trimmed();
    const QString cmd = l.section(' ', 0, 0).toUpper();
    const QString arg = l.section(' ', 1);

    if (cmd == "USER") {
        s->user = arg;
        send(s->ctrl, "331 Password required");
    } else if (cmd == "PASS") {
        const bool needAuth = !m_user.isEmpty();
        if (!needAuth) {
            s->loggedIn = true;
            send(s->ctrl, "230 Logged in");
        } else if (s->user == m_user && arg == m_pass) {
            s->loggedIn = true;
            send(s->ctrl, "230 Logged in");
        } else {
            send(s->ctrl, "530 Authentication required");
        }
    } else if (!s->loggedIn) {
        send(s->ctrl, "530 Please login");
    } else if (cmd == "TYPE") {
        send(s->ctrl, "200 Type set");
    } else if (cmd == "SIZE") {
        if (m_missing)     send(s->ctrl, "550 No such file");
        else if (m_noSize) send(s->ctrl, "502 SIZE not implemented");
        else               send(s->ctrl, QString("213 %1").arg(m_content.size()));
    } else if (cmd == "MDTM") {
        if (m_missing)     send(s->ctrl, "550 No such file");
        else if (m_noMdtm) send(s->ctrl, "502 MDTM not implemented");
        else               send(s->ctrl, QString("213 %1").arg(m_mdtm));
    } else if (cmd == "PASV") {
        if (s->pasv) { s->pasv->deleteLater(); s->pasv = nullptr; }
        s->pasv = new QTcpServer(this);
        if (!s->pasv->listen(QHostAddress::LocalHost, 0)) {
            send(s->ctrl, "425 Can't open data connection");
            return;
        }
        const quint16 p = s->pasv->serverPort();
        send(s->ctrl, QString("227 Entering Passive Mode (127,0,0,1,%1,%2).")
                          .arg(p / 256).arg(p % 256));
    } else if (cmd == "REST") {
        const bool fails = m_noRest ||
                           (m_restFailsAt > 0 && s->index >= m_restFailsAt);
        if (fails) { send(s->ctrl, "502 REST not implemented"); return; }
        s->rest = arg.toLongLong();
        send(s->ctrl, QString("350 Restarting at %1").arg(s->rest));
    } else if (cmd == "RETR") {
        if (m_missing) { send(s->ctrl, "550 No such file"); return; }
        if (!s->pasv)  { send(s->ctrl, "425 Use PASV first"); return; }
        startTransfer(s);
    } else if (cmd == "QUIT") {
        send(s->ctrl, "221 Bye");
        s->ctrl->disconnectFromHost();
    } else {
        send(s->ctrl, "500 Unknown command");
    }
}

void TestFtpServer::startTransfer(Session* s) {
    send(s->ctrl, "150 Opening data connection");

    auto deliver = [this, s] {
        s->data = s->pasv->nextPendingConnection();
        if (!s->data) return;

        QByteArray payload = m_content.mid(s->rest);   // FTP manda do REST ATÉ O FIM

        // dropAfter: mata a transferência no meio. Com dropOnce, só a PRIMEIRA
        // morre — as seguintes completam, o que permite testar um retry
        // BEM-SUCEDIDO (e não só a desistência após esgotar retries).
        const bool doDrop = (m_dropAfter >= 0) && payload.size() > m_dropAfter &&
                            (!m_dropOnce || !m_dropped);
        if (doDrop) {
            m_dropped = true;
            payload = payload.left(m_dropAfter);
            s->data->write(payload);
            s->data->flush();
            s->data->abort();                          // queda no meio: sem 226
            s->rest = 0;
            return;
        }
        s->data->write(payload);
        s->data->flush();
        s->data->disconnectFromHost();
        send(s->ctrl, "226 Transfer complete");
        s->rest = 0;
    };

    if (s->pasv->hasPendingConnections()) deliver();
    else connect(s->pasv, &QTcpServer::newConnection, this, deliver, Qt::SingleShotConnection);
}

#include "SingleInstanceGuard.h"
#include <QLocalServer>
#include <QLocalSocket>

SingleInstanceGuard::SingleInstanceGuard(QString serverName, QObject* parent)
    : QObject(parent), m_name(std::move(serverName)) {}

bool SingleInstanceGuard::tryBecomePrimary(const QByteArray& secondaryMessage) {
    // Probe for an existing primary.
    QLocalSocket probe;
    probe.connectToServer(m_name);
    if (probe.waitForConnected(200)) {
        probe.write(secondaryMessage);
        probe.flush();
        probe.waitForBytesWritten(200);
        probe.disconnectFromServer();
        return false;                          // a primary already runs
    }
    // Become the primary. removeServer clears a stale socket left by a crash.
    QLocalServer::removeServer(m_name);
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this,
            &SingleInstanceGuard::onNewConnection);
    m_server->listen(m_name);                  // best-effort; failure -> no IPC, still primary
    return true;
}

void SingleInstanceGuard::onNewConnection() {
    while (QLocalSocket* sock = m_server->nextPendingConnection()) {
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] {
            const QByteArray msg = sock->readAll();
            if (msg.contains(kShowMessage))
                emit showRequested();
            sock->disconnectFromServer();
            sock->deleteLater();
        });
        connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
    }
}

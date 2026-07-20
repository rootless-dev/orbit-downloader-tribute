#include "BrowserBridge.h"
#include <QTcpServer>
#include <QTcpSocket>

BrowserBridge::BrowserBridge(QObject* parent) : QObject(parent) {}

bool BrowserBridge::start(quint16 port, const QString& token, const QString& allowedOrigin) {
    stop();
    m_token = token;
    m_allowedOrigin = allowedOrigin;
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &BrowserBridge::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        delete m_server; m_server = nullptr;
        return false;                       // porta ocupada
    }
    return true;
}

void BrowserBridge::stop() {
    m_buffers.clear();
    if (m_server) {
        m_server->close();          // libera o socket de escuta JÁ, não no próximo turno do loop
        m_server->deleteLater();
        m_server = nullptr;
    }
}

bool    BrowserBridge::listening() const { return m_server && m_server->isListening(); }
quint16 BrowserBridge::port() const      { return m_server ? m_server->serverPort() : 0; }

void BrowserBridge::onNewConnection() {
    while (QTcpSocket* sock = m_server->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
            m_buffers.remove(sock); sock->deleteLater();
        });
    }
}

void BrowserBridge::onReadyRead(QTcpSocket* sock) {
    QByteArray& buf = m_buffers[sock];
    buf += sock->readAll();
    if (buf.size() > kMaxRequestBytes) {                 // guarda de tamanho
        sendJson(sock, 413, "Payload Too Large", R"({"ok":false,"error":"too_large"})");
        return;
    }
    const AddRequest req = parseAddRequest(buf);
    if (!req.headersComplete) return;                    // aguarda mais bytes
    if (req.method == "OPTIONS") { sendPreflight(sock); return; }
    if (req.method != "POST" || req.path != "/add") {
        sendJson(sock, 404, "Not Found", R"({"ok":false,"error":"not_found"})"); return;
    }
    if (!req.bodyComplete) return;                       // aguarda o corpo inteiro

    switch (authorize(req, m_token, m_allowedOrigin)) {
        case AuthResult::Forbidden:
            sendJson(sock, 403, "Forbidden", R"({"ok":false,"error":"forbidden"})"); return;
        case AuthResult::Unauthorized:
            sendJson(sock, 401, "Unauthorized", R"({"ok":false,"error":"unauthorized"})"); return;
        case AuthResult::Ok: break;
    }
    const DownloadPayload p = parseBody(req.body);
    if (!p.valid) {
        sendJson(sock, 400, "Bad Request", R"({"ok":false,"error":"bad_request"})"); return;
    }
    emit downloadRequested(p.url, headersFromPayload(p), p.filename);
    sendJson(sock, 200, "OK", R"({"ok":true})");
}

QByteArray BrowserBridge::corsBlock() const {
    return "Access-Control-Allow-Origin: " + m_allowedOrigin.toUtf8() + "\r\n"
           "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, X-Orbit-Token\r\n"
           "Access-Control-Allow-Private-Network: true\r\n"
           "Access-Control-Max-Age: 600\r\nVary: Origin\r\n";
}

void BrowserBridge::sendPreflight(QTcpSocket* sock) {
    sock->write("HTTP/1.1 204 No Content\r\n" + corsBlock() + "Content-Length: 0\r\n\r\n");
    sock->flush();
    m_buffers.remove(sock);
    sock->disconnectFromHost();
}

void BrowserBridge::sendJson(QTcpSocket* sock, int status, const char* reason,
                             const QByteArray& body) {
    sock->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n"
                + corsBlock() + "Content-Type: application/json\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    sock->flush();
    m_buffers.remove(sock);
    sock->disconnectFromHost();
}

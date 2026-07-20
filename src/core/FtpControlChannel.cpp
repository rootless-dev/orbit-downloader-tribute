#include "FtpControlChannel.h"
#include "FtpReply.h"
#include <QTcpSocket>
#include <QTimer>

FtpControlChannel::FtpControlChannel(QObject* parent) : QObject(parent) {
    qRegisterMetaType<FtpErrorClass>("FtpErrorClass");
}

void FtpControlChannel::connectAndLogin(const QUrl& url, const Credentials& creds,
                                        int connectTimeoutMs) {
    m_url       = url;
    m_creds     = creds;
    m_timeoutMs = connectTimeoutMs;
    m_phase     = Phase::Greeting;
    m_dead      = false;
    m_buf.clear();

    m_sock = new QTcpSocket(this);
    connect(m_sock, &QTcpSocket::connected,     this, &FtpControlChannel::onConnected);
    connect(m_sock, &QTcpSocket::readyRead,     this, &FtpControlChannel::onReadyRead);
    connect(m_sock, &QTcpSocket::errorOccurred, this, &FtpControlChannel::onSocketError);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &FtpControlChannel::onTimeout);
    m_timer->start(m_timeoutMs);

    m_sock->connectToHost(m_url.host(), quint16(m_url.port(21)));
}

void FtpControlChannel::onConnected() {
    // Nada a fazer: o servidor fala primeiro (220). O timer segue armado até o
    // greeting chegar.
}

void FtpControlChannel::write(const QString& cmd) {
    if (!m_sock) return;
    m_sock->write((cmd + "\r\n").toUtf8());
    m_sock->flush();
    if (m_timer) m_timer->start(m_timeoutMs);   // re-arma a cada comando
}

void FtpControlChannel::onReadyRead() {
    if (m_dead || !m_sock) return;
    m_buf += m_sock->readAll();
    while (true) {
        int consumed = 0;
        const FtpReply r = parseReply(m_buf, &consumed);
        if (!r.complete) break;
        m_buf.remove(0, consumed);
        handleReply(r.code, r.text);
        if (m_dead) return;
    }
}

FtpErrorClass FtpControlChannel::classify(int code) {
    switch (code) {
        case 421: case 425: case 426: return FtpErrorClass::Transient;
        case 530:                     return FtpErrorClass::Auth;
        default:                      return FtpErrorClass::Fatal;
    }
}

void FtpControlChannel::handleReply(int code, const QString& text) {
    if (m_timer) m_timer->stop();

    switch (m_phase) {
        case Phase::Greeting:
            if (code != 220) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::User;
            write(m_creds.user.isEmpty() ? "USER anonymous"
                                         : "USER " + m_creds.user);
            return;

        case Phase::User:
            if (code == 230) { m_phase = Phase::Type; write("TYPE I"); return; }   // sem senha
            if (code != 331) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Pass;
            write(m_creds.pass.isEmpty() ? "PASS orbit@tribute"
                                         : "PASS " + m_creds.pass);
            return;

        case Phase::Pass:
            if (code != 230) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Type;
            write("TYPE I");
            return;

        case Phase::Type:
            if (code != 200) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Ready;
            emit loggedIn();
            return;

        case Phase::Ready:
            emit replyReceived(code, text);
            return;

        case Phase::Idle:
            return;
    }
}

void FtpControlChannel::sendCommand(const QString& cmd) {
    if (m_phase != Phase::Ready || m_dead) return;
    write(cmd);
}

void FtpControlChannel::onSocketError() {
    if (m_dead || !m_sock) return;
    // Erro de socket (conexão recusada, reset, host inacessível) é sempre
    // transitório do ponto de vista do worker: vale um retry com backoff.
    fail("socket: " + m_sock->errorString(), FtpErrorClass::Transient);
}

void FtpControlChannel::onTimeout() {
    if (m_dead) return;
    fail("timeout", FtpErrorClass::Transient);
}

void FtpControlChannel::fail(const QString& error, FtpErrorClass cls) {
    if (m_dead) return;
    m_dead = true;
    if (m_timer) m_timer->stop();
    emit failed(error, cls);
}

void FtpControlChannel::close() {
    m_dead  = true;
    m_phase = Phase::Idle;
    if (m_timer) m_timer->stop();
    if (m_sock) {
        m_sock->disconnect(this);       // nada de sinais depois do close
        m_sock->abort();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
}

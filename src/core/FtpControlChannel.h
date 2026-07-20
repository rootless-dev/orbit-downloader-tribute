#pragma once
#include "Transport.h"          // Credentials
#include <QObject>
#include <QQueue>
#include <QUrl>
class QTcpSocket;
class QTimer;

enum class FtpErrorClass { Transient, Fatal, Auth };

// Socket de controle FTP: conecta, faz login (USER/PASS/TYPE I) e serializa
// comandos -> respostas. Compartilhado por FtpProbe e FtpSegmentWorker.
class FtpControlChannel : public QObject {
    Q_OBJECT
public:
    explicit FtpControlChannel(QObject* parent = nullptr);

    void connectAndLogin(const QUrl& url, const Credentials& creds, int connectTimeoutMs);
    void sendCommand(const QString& cmd);
    void close();

signals:
    void loggedIn();
    void replyReceived(int code, const QString& text);
    void failed(const QString& error, FtpErrorClass cls);

private:
    enum class Phase { Idle, Greeting, User, Pass, Type, Ready };
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onTimeout();
    void handleReply(int code, const QString& text);
    void write(const QString& cmd);
    void fail(const QString& error, FtpErrorClass cls);
    static FtpErrorClass classify(int code);

    QTcpSocket* m_sock = nullptr;
    QTimer*     m_timer = nullptr;
    QByteArray  m_buf;
    QUrl        m_url;
    Credentials m_creds;
    Phase       m_phase = Phase::Idle;
    int         m_timeoutMs = 30000;
    bool        m_dead = false;
};

Q_DECLARE_METATYPE(FtpErrorClass)

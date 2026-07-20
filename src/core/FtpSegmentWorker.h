#pragma once
#include "FtpControlChannel.h"
#include "Transport.h"
#include <QUrl>
class QFile;
class QTcpSocket;
class QTimer;
class RateLimiter;

class FtpSegmentWorker : public SegmentSource {
    Q_OBJECT
public:
    FtpSegmentWorker(QFile* file, const EngineConfig& cfg, RateLimiter* limiter = nullptr,
                      QObject* parent = nullptr);
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds,
               const HeaderList& extraHeaders) override;
    void stop() override;
    Segment segment() const override { return m_seg; }

private:
    enum class Step { Mdtm, Pasv, Rest, Retr, Transferring };
    void openAttempt();
    void onLoggedIn();
    void onReply(int code, const QString& text);
    void onControlFailed(const QString& error, FtpErrorClass cls);
    void onDataReadyRead();
    void onDataFinished();
    void finishSegment();
    void scheduleRetry(const QString& why);
    void armIdleTimer(int ms);
    void onTimeout();
    void teardown();
    void scheduleDrain();

    QFile*             m_file;
    EngineConfig       m_cfg;
    RateLimiter*       m_limiter = nullptr;
    Segment            m_seg;
    QUrl               m_url;
    QString            m_validator;
    Credentials        m_creds;
    FtpControlChannel* m_ch   = nullptr;
    QTcpSocket*        m_data = nullptr;
    Step               m_step = Step::Pasv;
    bool               m_stopped = false;
    bool               m_finished = false;
    int                m_attempt = 0;
    QTimer*            m_retryTimer = nullptr;
    QTimer*            m_idleTimer  = nullptr;
    QTimer*            m_drainTimer = nullptr;
};

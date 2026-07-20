#pragma once
#include "Transport.h"
#include <QNetworkReply>   // needed for the QNetworkReply::NetworkError enum used below
class QNetworkAccessManager;
class QFile;
class QTimer;
class RateLimiter;

class SegmentWorker : public SegmentSource {
    Q_OBJECT
public:
    SegmentWorker(QNetworkAccessManager* nam, QFile* file, const EngineConfig& cfg,
                  RateLimiter* limiter = nullptr, QObject* parent = nullptr);
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds,
               const HeaderList& extraHeaders) override;
    void stop() override;
    Segment segment() const override { return m_seg; }
private:
    void openRequest();
    void onReadyRead();
    void onFinished();
    void onErrorOccurred();
    bool isRecoverable(QNetworkReply::NetworkError e) const;
    void scheduleRetry(const QString& why);
    void armIdleTimer(int ms);
    void onTimeout();
    void scheduleDrain();

    QNetworkAccessManager* m_nam;
    QFile*                 m_file;
    EngineConfig           m_cfg;
    RateLimiter*           m_limiter = nullptr;
    Segment                m_seg;
    QUrl                   m_url;
    QString                m_validator;
    HeaderList             m_extraHeaders;
    QNetworkReply*         m_reply    = nullptr;
    bool                   m_expectPartial = false;
    bool                   m_stopped  = false;
    int                    m_attempt  = 0;
    QTimer*                m_retryTimer = nullptr;
    QTimer*                m_idleTimer  = nullptr;
    QTimer*                m_drainTimer = nullptr;
};

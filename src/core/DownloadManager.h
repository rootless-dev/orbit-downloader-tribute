#pragma once
#include "DownloadTypes.h"
#include "DownloadTask.h"
#include "Transport.h"
#include "RateLimiter.h"
#include <QObject>
#include <QVector>
#include <QHash>
#include <memory>
#include <vector>

class Logger;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                    Logger* logger = nullptr, QObject* parent = nullptr);
    QUuid addDownload(const QUrl& url, const QString& destPath, const HeaderList& extraHeaders = {},
                      bool provisionalName = false);
    void  pauseAll();
    void  resumeAll();
    void  remove(const QUuid& id, bool deleteFiles);
    void  loadSession();
    void  setConfig(const EngineConfig& cfg);   // banda + cap ao vivo; resto p/ próximos downloads
    QVector<DownloadTask*> tasks() const { return m_tasks; }
    DownloadTask* taskById(const QUuid& id) const;
    void  pause(const QUuid& id);
    void  resume(const QUuid& id);
    void  cancel(const QUuid& id);
    void  setPriority(const QUuid& id, Priority p);
    bool  moveFiles(const QUuid& id, const QString& newDir);
    bool  retarget(const QUuid& id, const QString& newDestPath);
    void  provideCredentials(const QUuid& id, const QString& user, const QString& pass);
    Transport* transportFor(const QUrl& url) const;   // nullptr se esquema desconhecido
signals:
    void taskProgress(const QUuid& id, qint64 received, qint64 total);
    void taskStateChanged(const QUuid& id, DownloadState state);
    void credentialsRequired(const QUuid& id, const QString& host);
private:
    QString sessionPath() const;
    void    saveSession();
    void    pump();                 // promote Queued -> Downloading up to maxConcurrent
    void    wire(DownloadTask* t);

    EngineConfig            m_cfg;
    QString                 m_dataDir;
    RateLimiter             m_limiter;       // teto global de banda, consultado pelos workers
    QHash<QString, Transport*>              m_transports;   // scheme -> transport (não-dono)
    std::vector<std::unique_ptr<Transport>> m_owned;        // dono de verdade
    QVector<DownloadTask*>  m_tasks;
    bool                    m_inPump = false;
    Logger*                 m_logger = nullptr;   // não-dono; pode ser nullptr
};

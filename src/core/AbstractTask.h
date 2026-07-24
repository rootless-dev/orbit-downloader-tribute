#pragma once
#include "DownloadTypes.h"
#include <QObject>
#include <QUuid>

// Task 6: base interface extracted from DownloadTask so DownloadManager (and
// the GUI models) can hold heterogeneous task kinds (HTTP/FTP today, Torrent
// soon) behind one pointer type. Pure virtual - DownloadTask is currently the
// only implementation; the pump()/table-model plumbing goes through this
// interface instead of the concrete class wherever it doesn't need
// DownloadTask-specific state (segments/record/credentials/etc.).
class AbstractTask : public QObject {
    Q_OBJECT
public:
    enum class Kind { Http, Ftp, Torrent };
    explicit AbstractTask(QObject* parent = nullptr) : QObject(parent) {}
    virtual Kind    kind() const = 0;
    virtual QUuid   id() const = 0;
    virtual DownloadState state() const = 0;
    virtual QString displayName() const = 0;   // file/torrent name shown in the table
    virtual qint64  totalBytes() const = 0;     // -1 until known
    virtual qint64  receivedBytes() const = 0;
    virtual Priority priority() const = 0;
    virtual void    setPriority(Priority p) = 0;
    virtual void    start() = 0;
    virtual void    pause() = 0;
    virtual void    requeue() = 0;
    virtual void    cancel() = 0;
signals:
    void progress(qint64 received, qint64 total);
    void stateChanged(DownloadState state);
};

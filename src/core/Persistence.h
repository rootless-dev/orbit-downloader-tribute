#pragma once
#include "DownloadTypes.h"
#include <QJsonObject>

struct DownloadRecord {
    QUuid         id;
    QUrl          url;
    QString       destPath;
    qint64        totalBytes    = -1;
    bool          supportsRange = false;
    DownloadState state         = DownloadState::Queued;
    int           segmentCount  = 4;
    Priority      priority      = Priority::Normal;
    HeaderList    extraHeaders;   // headers per-download (cookies/referer/UA from browser)
};

namespace Persistence {
    bool    writeFileAtomic(const QString& path, const QByteArray& data);
    QString metaPath(const QString& destPath);
    bool    writeMeta(const QString& destPath, const QVector<Segment>& segs,
                      const QString& etag, const QString& lastModified, bool validated);
    bool    readMeta(const QString& destPath, QVector<Segment>& segs,
                     QString& etag, QString& lastModified, bool& validated);
    void    removeMeta(const QString& destPath);
    bool    writeSession(const QString& jsonPath, const QVector<DownloadRecord>& recs);
    QVector<DownloadRecord> readSession(const QString& jsonPath);
    QString resolveUniquePath(const QString& destPath);
    QJsonObject readJsonObject(const QString& path);
    bool        writeJsonObject(const QString& path, const QJsonObject& obj);
}

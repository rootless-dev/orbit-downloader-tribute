#include "Persistence.h"
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace Persistence {

bool writeFileAtomic(const QString& path, const QByteArray& data) {
    QSaveFile f(path);                       // QSaveFile = temp + atomic rename on commit
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(data) != data.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

QString metaPath(const QString& destPath) { return destPath + ".meta"; }

static QJsonObject segToJson(const Segment& s) {
    return QJsonObject{{"index", s.index}, {"start", double(s.start)},
                       {"current", double(s.current)}, {"end", double(s.end)}};
}
static Segment segFromJson(const QJsonObject& o) {
    Segment s;
    s.index   = o.value("index").toInt();
    s.start   = qint64(o.value("start").toDouble());
    s.current = qint64(o.value("current").toDouble());
    s.end     = qint64(o.value("end").toDouble());
    return s;
}

bool writeMeta(const QString& destPath, const QVector<Segment>& segs,
               const QString& etag, const QString& lastModified, bool validated) {
    QJsonArray arr;
    for (const auto& s : segs) arr.append(segToJson(s));
    QJsonObject root{{"segments", arr}, {"etag", etag},
                     {"lastModified", lastModified}, {"validated", validated}};
    return writeFileAtomic(metaPath(destPath),
                           QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool readMeta(const QString& destPath, QVector<Segment>& segs,
              QString& etag, QString& lastModified, bool& validated) {
    QFile f(metaPath(destPath));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    const auto root = doc.object();
    segs.clear();
    for (const auto v : root.value("segments").toArray()) segs.append(segFromJson(v.toObject()));
    etag         = root.value("etag").toString();
    lastModified = root.value("lastModified").toString();
    validated    = root.value("validated").toBool();
    return true;
}

void removeMeta(const QString& destPath) { QFile::remove(metaPath(destPath)); }

bool writeSession(const QString& jsonPath, const QVector<DownloadRecord>& recs) {
    QJsonArray arr;
    for (const auto& r : recs) {
        QJsonArray hdrs;
        for (const auto& h : r.extraHeaders)
            hdrs.append(QJsonObject{{"name",  QString::fromUtf8(h.first)},
                                    {"value", QString::fromUtf8(h.second)}});
        arr.append(QJsonObject{
            {"id", r.id.toString(QUuid::WithoutBraces)},
            {"url", r.url.toString()},
            {"destPath", r.destPath},
            {"totalBytes", double(r.totalBytes)},
            {"supportsRange", r.supportsRange},
            {"state", int(r.state)},
            {"segmentCount", r.segmentCount},
            {"priority", priorityToString(r.priority)},
            {"headers", hdrs}});
    }
    return writeFileAtomic(jsonPath, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<DownloadRecord> readSession(const QString& jsonPath) {
    QVector<DownloadRecord> recs;
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) return recs;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    for (const auto v : doc.array()) {
        const auto o = v.toObject();
        DownloadRecord r;
        r.id            = QUuid::fromString(o.value("id").toString());
        r.url           = QUrl(o.value("url").toString());
        r.destPath      = o.value("destPath").toString();
        r.totalBytes    = qint64(o.value("totalBytes").toDouble());
        r.supportsRange = o.value("supportsRange").toBool();
        r.state         = DownloadState(o.value("state").toInt());
        r.segmentCount  = o.value("segmentCount").toInt();
        r.priority      = priorityFromString(o.value("priority").toString("Normal"));
        for (const QJsonValue& hv : o.value("headers").toArray()) {
            const QJsonObject h = hv.toObject();
            r.extraHeaders.append({h.value("name").toString().toUtf8(),
                                   h.value("value").toString().toUtf8()});
        }
        recs.append(r);
    }
    return recs;
}

QString resolveUniquePath(const QString& destPath) {
    if (!QFileInfo::exists(destPath)) return destPath;
    QFileInfo fi(destPath);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();       // "movie" from "movie.flv"
    const QString suf  = fi.suffix();                 // "flv"
    for (int i = 1; ; ++i) {
        QString candidate = dir + "/" + base + QString(" (%1)").arg(i);
        if (!suf.isEmpty()) candidate += "." + suf;
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

QJsonObject readJsonObject(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

bool writeJsonObject(const QString& path, const QJsonObject& obj) {
    return writeFileAtomic(path, QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

} // namespace Persistence

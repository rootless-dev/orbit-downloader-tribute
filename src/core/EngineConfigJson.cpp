#include "EngineConfigJson.h"

static int getInt(const QJsonObject& o, const char* k, int def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt(def) : def;
}
static qint64 getI64(const QJsonObject& o, const char* k, qint64 def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? qint64(v.toDouble(double(def))) : def;
}
static QString getStr(const QJsonObject& o, const char* k, const QString& def) {
    const auto v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : def;
}

QJsonObject engineConfigToJson(const EngineConfig& c) {
    return QJsonObject{
        {"maxConcurrentDownloads", c.maxConcurrentDownloads},
        {"segmentCount",           c.segmentCount},
        {"minSegSize",             double(c.minSegSize)},
        {"maxSegmentRetries",      c.maxSegmentRetries},
        {"retryBackoffBaseMs",     c.retryBackoffBaseMs},
        {"connectTimeoutMs",       c.connectTimeoutMs},
        {"idleTimeoutMs",          c.idleTimeoutMs},
        {"progressThrottleMs",     c.progressThrottleMs},
        {"maxBytesPerSec",         double(c.maxBytesPerSec)},
        {"userAgent",              c.userAgent}};
}

EngineConfig engineConfigFromJson(const QJsonObject& o, const EngineConfig& d) {
    EngineConfig c;
    c.maxConcurrentDownloads = getInt(o, "maxConcurrentDownloads", d.maxConcurrentDownloads);
    c.segmentCount           = getInt(o, "segmentCount",           d.segmentCount);
    c.minSegSize             = getI64(o, "minSegSize",             d.minSegSize);
    c.maxSegmentRetries      = getInt(o, "maxSegmentRetries",      d.maxSegmentRetries);
    c.retryBackoffBaseMs     = getInt(o, "retryBackoffBaseMs",     d.retryBackoffBaseMs);
    c.connectTimeoutMs       = getInt(o, "connectTimeoutMs",       d.connectTimeoutMs);
    c.idleTimeoutMs          = getInt(o, "idleTimeoutMs",          d.idleTimeoutMs);
    c.progressThrottleMs     = getInt(o, "progressThrottleMs",     d.progressThrottleMs);
    c.maxBytesPerSec         = getI64(o, "maxBytesPerSec",         d.maxBytesPerSec);
    c.userAgent              = getStr(o, "userAgent",              d.userAgent);
    return c;
}

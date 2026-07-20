#include "BrowserBridgeProtocol.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

static QString headerValue(const QByteArray& headerBlock, const char* name) {
    for (const QByteArray& line : headerBlock.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        if (line.left(colon).trimmed().toLower() == QByteArray(name).toLower())
            return QString::fromUtf8(line.mid(colon + 1).trimmed());
    }
    return QString();
}

AddRequest parseAddRequest(const QByteArray& raw) {
    AddRequest r;
    const int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) return r;                 // headers incompletos
    r.headersComplete = true;
    const QByteArray head = raw.left(sep);
    const int firstEol = head.indexOf("\r\n");
    const QByteArray requestLine = firstEol < 0 ? head : head.left(firstEol);
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() >= 2) { r.method = QString::fromUtf8(parts[0]);
                             r.path   = QString::fromUtf8(parts[1]); }
    const QByteArray headers = firstEol < 0 ? QByteArray() : head.mid(firstEol + 2);
    r.origin = headerValue(headers, "Origin");
    r.token  = headerValue(headers, "X-Orbit-Token");
    const QString cl = headerValue(headers, "Content-Length");
    r.contentLength = cl.isEmpty() ? -1 : cl.toLongLong();
    r.body = raw.mid(sep + 4);
    r.bodyComplete = (r.contentLength >= 0)
        ? (r.body.size() >= r.contentLength)
        : true;                            // sem corpo declarado: nada a esperar
    if (r.contentLength >= 0) r.body = r.body.left(r.contentLength);
    return r;
}

DownloadPayload parseBody(const QByteArray& json) {
    DownloadPayload p;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QUrl u(o.value("url").toString());
    const QString scheme = u.scheme().toLower();
    if (!u.isValid() || (scheme != "http" && scheme != "https")) return p;
    p.url       = u;
    p.filename  = o.value("filename").toString();
    p.referrer  = o.value("referrer").toString();
    p.userAgent = o.value("userAgent").toString();
    p.cookie    = o.value("cookie").toString();
    p.valid     = true;
    return p;
}

HeaderList headersFromPayload(const DownloadPayload& p) {
    HeaderList h;
    if (!p.cookie.isEmpty())    h.append({"Cookie",     p.cookie.toUtf8()});
    if (!p.referrer.isEmpty())  h.append({"Referer",    p.referrer.toUtf8()});
    if (!p.userAgent.isEmpty()) h.append({"User-Agent", p.userAgent.toUtf8()});
    return h;
}

static bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    quint8 diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= quint8(a[i]) ^ quint8(b[i]);
    return diff == 0;
}

AuthResult authorize(const AddRequest& req, const QString& expectedToken,
                     const QString& allowedOrigin) {
    if (!req.origin.isEmpty() && req.origin != allowedOrigin)
        return AuthResult::Forbidden;
    if (expectedToken.isEmpty() ||
        !constantTimeEquals(req.token.toUtf8(), expectedToken.toUtf8()))
        return AuthResult::Unauthorized;
    return AuthResult::Ok;
}

QString generateBridgeToken() {
    quint32 buf[4];
    QRandomGenerator::system()->fillRange(buf);
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(buf), sizeof(buf)).toHex());
}

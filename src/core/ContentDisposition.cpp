#include "ContentDisposition.h"
#include <QFileInfo>
#include <QStringList>
#include <QUrl>

namespace {

QString sanitizeFileName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = QFileInfo(name).fileName();          // só o basename -> bloqueia ../../
    static const QString invalid = QStringLiteral("/\\:*?\"<>|");
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c.unicode() < 0x20) continue;       // caracteres de controle
        if (invalid.contains(c)) continue;
        out.append(c);
    }
    return out.trimmed();
}

// Divide "a=1; b=\"x;y\"; c=2" por ';' respeitando aspas duplas.
QStringList splitParams(const QString& s) {
    QStringList parts;
    QString cur;
    bool inQuotes = false;
    QChar prev;
    for (const QChar c : s) {
        if (c == QLatin1Char('"') && prev != QLatin1Char('\\')) { inQuotes = !inQuotes; cur.append(c); }
        else if (c == QLatin1Char(';') && !inQuotes) { parts.append(cur); cur.clear(); }
        else cur.append(c);
        prev = c;
    }
    parts.append(cur);
    return parts;
}

// filename*=  ->  charset'lang'valor-percent-encoded
QString decodeExtended(const QString& v) {
    const int q1 = v.indexOf(QLatin1Char('\''));
    if (q1 < 0) return QString();
    const int q2 = v.indexOf(QLatin1Char('\''), q1 + 1);
    if (q2 < 0) return QString();
    const QString encoded = v.mid(q2 + 1);
    // fromPercentEncoding decodifica os bytes como UTF-8 (charset comum). Um
    // charset exótico cairia em replacement chars — aceito pela spec.
    return QUrl::fromPercentEncoding(encoded.toUtf8());
}

QString stripQuotes(QString v) {
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
        v = v.mid(1, v.size() - 2);
    v.replace(QStringLiteral("\\\""), QStringLiteral("\""));   // desescapa \"
    return v;
}

} // namespace

QString parseContentDisposition(const QString& headerValue) {
    QString extended, plain;
    const QStringList params = splitParams(headerValue);
    for (const QString& raw : params) {
        const QString p = raw.trimmed();
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString key = p.left(eq).trimmed().toLower();
        const QString val = p.mid(eq + 1);
        if (key == QLatin1String("filename*"))     extended = decodeExtended(val.trimmed());
        else if (key == QLatin1String("filename")) plain    = stripQuotes(val);
    }
    const QString e = sanitizeFileName(extended);
    return !e.isEmpty() ? e : sanitizeFileName(plain);
}

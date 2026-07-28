#include "torrent/MagnetUri.h"
#include <QUrl>
#include <QUrlQuery>

namespace {
// RFC 4648 base32 decode (uppercase A-Z 2-7). Returns empty on invalid input.
QByteArray base32Decode(const QString& s) {
    static const QString A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QByteArray out; int buffer = 0, bits = 0;
    for (QChar c : s.toUpper()) {
        int v = A.indexOf(c);
        if (v < 0) return {};
        buffer = (buffer << 5) | v; bits += 5;
        if (bits >= 8) { bits -= 8; out.append(char((buffer >> bits) & 0xFF)); }
    }
    return out;
}
QByteArray decodeInfoHash(const QString& xt) {
    // xt = "urn:btih:<hex40|base32-32>"
    const QString prefix = "urn:btih:";
    if (!xt.startsWith(prefix)) return {};
    const QString h = xt.mid(prefix.size());
    if (h.size() == 40) {
        const QByteArray raw = QByteArray::fromHex(h.toLatin1());
        return raw.size() == 20 ? raw : QByteArray();
    }
    if (h.size() == 32) {
        const QByteArray raw = base32Decode(h);
        return raw.size() == 20 ? raw : QByteArray();
    }
    return {};
}
} // namespace

MagnetInfo MagnetUri::parse(const QString& uri) {
    MagnetInfo m;
    if (!uri.startsWith("magnet:?")) return m;
    const QUrlQuery q(uri.mid(QString("magnet:?").size()));
    for (const auto& kv : q.queryItems(QUrl::FullyDecoded)) {
        if (kv.first == "xt" && m.infoHash.isEmpty()) m.infoHash = decodeInfoHash(kv.second);
        else if (kv.first == "dn") m.displayName = kv.second;
        else if (kv.first == "tr") m.trackers.append(kv.second);
    }
    return m;
}

#include "torrent/HttpTrackerClient.h"

#include "torrent/Bencode.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

QByteArray eventName(TrackerEvent ev) {
    switch (ev) {
    case TrackerEvent::Started:   return "started";
    case TrackerEvent::Stopped:   return "stopped";
    case TrackerEvent::Completed: return "completed";
    case TrackerEvent::None:      break;
    }
    return QByteArray();
}

// Percent-encodes EVERY byte as "%XX", unconditionally. QByteArray::toPercentEncoding
// leaves alnum and "-._~" bytes as literal ASCII even with empty include/exclude sets,
// which is wrong for BitTorrent's info_hash/peer_id: those are raw byte strings, not
// text, and must round-trip byte-for-byte regardless of whether a given byte happens
// to look like a "safe" character.
QByteArray encodeEveryByte(const QByteArray& in) {
    static const char* hex = "0123456789ABCDEF";
    QByteArray out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 0xF];
    }
    return out;
}

} // namespace

QUrl TrackerProto::buildAnnounceUrl(const QUrl& base, const QByteArray& infoHash, const QByteArray& peerId,
                                     quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev) {
    // Encode every byte with no exceptions, so arbitrary (non-printable, but also
    // ordinary alnum/'-') info-hash/peer-id bytes survive the round trip untouched.
    const QByteArray encodedInfoHash = encodeEveryByte(infoHash);
    const QByteArray encodedPeerId = encodeEveryByte(peerId);

    QString query;
    query += QStringLiteral("info_hash=") + QString::fromLatin1(encodedInfoHash);
    query += QStringLiteral("&peer_id=") + QString::fromLatin1(encodedPeerId);
    query += QStringLiteral("&port=") + QString::number(port);
    query += QStringLiteral("&uploaded=0");
    query += QStringLiteral("&downloaded=") + QString::number(downloaded);
    query += QStringLiteral("&left=") + QString::number(left);
    query += QStringLiteral("&compact=1");
    const QByteArray ev8 = eventName(ev);
    if (!ev8.isEmpty()) query += QStringLiteral("&event=") + QString::fromLatin1(ev8);

    QUrl url(base);
    // The query is already fully percent-encoded above (including info_hash/
    // peer_id); TolerantMode accepts existing %XX sequences as-is instead of
    // re-encoding the '%' character, so it round-trips exactly.
    url.setQuery(query, QUrl::TolerantMode);
    return url;
}

bool TrackerProto::parseResponse(const QByteArray& body, QVector<PeerAddress>* peers, int* interval,
                                 int* minInterval, QString* failure) {
    bool ok = false;
    const BencodeValue root = Bencode::decode(body, &ok);
    if (!ok || root.type() != BencodeValue::Type::Dict) {
        if (failure) *failure = QStringLiteral("malformed tracker response");
        return false;
    }

    if (root.contains("failure reason")) {
        if (failure) *failure = QString::fromUtf8(root[QByteArray("failure reason")].toBytes());
        return false;
    }

    if (interval) *interval = root.contains("interval") ? int(root[QByteArray("interval")].toInt()) : 0;
    if (minInterval)
        *minInterval = root.contains("min interval") ? int(root[QByteArray("min interval")].toInt()) : 0;
    if (peers) peers->clear();

    if (root.contains("peers")) {
        const BencodeValue& peersValue = root[QByteArray("peers")];
        if (peersValue.type() == BencodeValue::Type::Bytes) {
            // Compact form: 6 bytes per peer (4-byte big-endian IPv4 + 2-byte
            // big-endian port).
            const QByteArray raw = peersValue.toBytes();
            // `i + 6 <= raw.size()` intentionally ignores any trailing partial
            // (<6 byte) record from a malformed tracker, rather than erroring
            // (lenient parsing).
            for (int i = 0; i + 6 <= raw.size(); i += 6) {
                const quint8 a = quint8(raw[i]);
                const quint8 b = quint8(raw[i + 1]);
                const quint8 c = quint8(raw[i + 2]);
                const quint8 d = quint8(raw[i + 3]);
                const quint16 port = quint16((quint8(raw[i + 4]) << 8) | quint8(raw[i + 5]));
                const QString host = QStringLiteral("%1.%2.%3.%4").arg(a).arg(b).arg(c).arg(d);
                if (peers) peers->append(PeerAddress{host, port});
            }
        } else if (peersValue.type() == BencodeValue::Type::List) {
            // Dictionary form: a list of { ip, peer id, port } dicts.
            for (const BencodeValue& entry : peersValue.toList()) {
                if (entry.type() != BencodeValue::Type::Dict) continue;
                const QString host = entry.contains("ip")
                    ? QString::fromUtf8(entry[QByteArray("ip")].toBytes())
                    : QString();
                const quint16 port = entry.contains("port")
                    ? quint16(entry[QByteArray("port")].toInt())
                    : 0;
                if (peers) peers->append(PeerAddress{host, port});
            }
        }
    }

    return true;
}

HttpTrackerClient::HttpTrackerClient(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_nam(nam) {}

void HttpTrackerClient::announce(const QUrl& tracker, const QByteArray& infoHash, const QByteArray& peerId,
                                  quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev) {
    if (tracker.scheme() == QLatin1String("udp")) {
        emit announceFailed(QStringLiteral("tracker uses UDP — supported in a later version"));
        return;
    }

    const QUrl url = TrackerProto::buildAnnounceUrl(tracker, infoHash, peerId, port, downloaded, left, ev);
    QNetworkReply* reply = m_nam->get(QNetworkRequest(url));
    // Reply cleanup must not depend on `this` (the client) still being alive: connect
    // it with `reply` itself as the context, so the reply always deletes itself on
    // completion even if HttpTrackerClient is destroyed mid-flight (which would
    // auto-disconnect the `this`-context lambda below and otherwise leak the reply
    // until `m_nam` is destroyed).
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit announceFailed(reply->errorString());
            return;
        }

        const QByteArray body = reply->readAll();
        QVector<PeerAddress> peers;
        int interval = 0;
        int minInterval = 0;
        QString failure;
        if (!TrackerProto::parseResponse(body, &peers, &interval, &minInterval, &failure)) {
            emit announceFailed(failure);
            return;
        }
        emit peersReceived(peers, interval, minInterval);
    });
}

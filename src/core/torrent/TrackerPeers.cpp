#include "torrent/TrackerPeers.h"

#include <QHostAddress>
#include <QtGlobal>

#include <algorithm>

namespace {

quint16 be16(const QByteArray& b, int i) {
    return quint16((quint8(b[i]) << 8) | quint8(b[i + 1]));
}

} // namespace

QVector<PeerAddress> TrackerPeers::fromCompactV4(const QByteArray& raw) {
    QVector<PeerAddress> out;
    for (int i = 0; i + 6 <= raw.size(); i += 6) {
        const QString host = QStringLiteral("%1.%2.%3.%4")
            .arg(quint8(raw[i])).arg(quint8(raw[i + 1]))
            .arg(quint8(raw[i + 2])).arg(quint8(raw[i + 3]));
        out.append(PeerAddress{host, be16(raw, i + 4)});
    }
    return out;
}

QVector<PeerAddress> TrackerPeers::fromCompactV6(const QByteArray& raw) {
    QVector<PeerAddress> out;
    for (int i = 0; i + 18 <= raw.size(); i += 18) {
        Q_IPV6ADDR addr;
        for (int j = 0; j < 16; ++j) addr[j] = quint8(raw[i + j]);
        const QString host = QHostAddress(addr).toString(); // bracketless literal
        out.append(PeerAddress{host, be16(raw, i + 16)});
    }
    return out;
}

bool TrackerPeers::isBogon(const PeerAddress& p) {
    if (p.port == 0) return true;
    QHostAddress a(p.host);
    if (a.isNull()) return true; // unparseable
    if (a == QHostAddress(QHostAddress::AnyIPv4) || a == QHostAddress(QHostAddress::AnyIPv6))
        return true; // 0.0.0.0 / ::
    if (a.isMulticast() || a.isBroadcast()) return true;
    // Loopback (127.0.0.0/8, ::1) is bogon in production -- a real tracker
    // reporting a loopback peer is malicious or broken. Test-only seam: an
    // offline in-process E2E needs to announce a loopback seeder through a
    // real tracker round trip, so ORBIT_ALLOW_LOOPBACK_PEERS keeps loopback
    // when explicitly set. Unset (the default, and always in production),
    // behavior is unchanged.
    if (a.isLoopback() && !qEnvironmentVariableIsSet("ORBIT_ALLOW_LOOPBACK_PEERS")) return true;
    if (a.protocol() == QAbstractSocket::IPv4Protocol && (a.toIPv4Address() >> 24) == 0)
        return true; // 0.0.0.0/8
    return false;
}

void TrackerPeers::dropBogons(QVector<PeerAddress>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const PeerAddress& p) { return isBogon(p); }),
            v.end());
}

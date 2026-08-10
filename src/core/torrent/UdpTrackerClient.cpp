#include "torrent/UdpTrackerClient.h"

#include "torrent/TrackerPeers.h"

#include <QHostAddress>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

#include <cstring>

quint32 UdpTrackerProto::eventCode(TrackerEvent ev) {
    switch (ev) {
    case TrackerEvent::None:      return 0;
    case TrackerEvent::Completed: return 1;
    case TrackerEvent::Started:   return 2;
    case TrackerEvent::Stopped:   return 3;
    }
    return 0;
}

QByteArray UdpTrackerProto::buildConnectRequest(quint32 txId) {
    QByteArray b(16, char(0));
    auto* p = reinterpret_cast<uchar*>(b.data());
    qToBigEndian<quint64>(kProtocolId, p);
    qToBigEndian<quint32>(0u, p + 8);   // action = connect
    qToBigEndian<quint32>(txId, p + 12);
    return b;
}

bool UdpTrackerProto::parseConnectResponse(const QByteArray& dg, quint32 expectTxId, quint64* connId) {
    if (dg.size() < 16) return false;
    const auto* p = reinterpret_cast<const uchar*>(dg.constData());
    if (qFromBigEndian<quint32>(p) != 0u) return false;                 // action must be connect
    if (qFromBigEndian<quint32>(p + 4) != expectTxId) return false;
    if (connId) *connId = qFromBigEndian<quint64>(p + 8);
    return true;
}

QByteArray UdpTrackerProto::buildAnnounceRequest(quint64 connId, quint32 txId, const QByteArray& infoHash,
                                                 const QByteArray& peerId, qint64 downloaded, qint64 left,
                                                 qint64 uploaded, TrackerEvent ev, quint32 key, quint16 port) {
    QByteArray b(98, char(0));
    auto* p = reinterpret_cast<uchar*>(b.data());
    qToBigEndian<quint64>(connId, p);
    qToBigEndian<quint32>(1u, p + 8);    // action = announce
    qToBigEndian<quint32>(txId, p + 12);
    // info_hash[20] @16, peer_id[20] @36 — copy exactly 20 bytes each (defensive).
    memcpy(b.data() + 16, infoHash.constData(), qMin(20, infoHash.size()));
    memcpy(b.data() + 36, peerId.constData(), qMin(20, peerId.size()));
    qToBigEndian<quint64>(quint64(downloaded), p + 56);
    qToBigEndian<quint64>(quint64(left), p + 64);
    qToBigEndian<quint64>(quint64(uploaded), p + 72);
    qToBigEndian<quint32>(eventCode(ev), p + 80);
    qToBigEndian<quint32>(0u, p + 84);   // IP = 0 (tracker uses source addr)
    qToBigEndian<quint32>(key, p + 88);
    qToBigEndian<qint32>(-1, p + 92);    // num_want = -1 (default)
    qToBigEndian<quint16>(port, p + 96);
    return b;
}

bool UdpTrackerProto::parseAnnounceResponse(const QByteArray& dg, quint32 expectTxId, int* intervalSecs,
                                            QVector<PeerAddress>* peers, QString* errorMsg,
                                            bool* isTrackerError) {
    if (isTrackerError) *isTrackerError = false;
    if (dg.size() < 8) { if (errorMsg) *errorMsg = QStringLiteral("short datagram"); return false; }
    const auto* p = reinterpret_cast<const uchar*>(dg.constData());
    const quint32 action = qFromBigEndian<quint32>(p);
    if (qFromBigEndian<quint32>(p + 4) != expectTxId) {
        if (errorMsg) *errorMsg = QStringLiteral("transaction id mismatch");
        return false;
    }
    if (action == 3u) { // error, and txId matches: a genuine tracker-reported error
        if (errorMsg) *errorMsg = QString::fromUtf8(dg.mid(8));
        if (isTrackerError) *isTrackerError = true;
        return false;
    }
    if (action != 1u || dg.size() < 20) {
        if (errorMsg) *errorMsg = QStringLiteral("malformed announce response");
        return false;
    }
    if (intervalSecs) *intervalSecs = int(qFromBigEndian<quint32>(p + 8));
    // leechers @12, seeders @16, peers (compact v4) from @20.
    if (peers) {
        *peers = TrackerPeers::fromCompactV4(dg.mid(20));
        TrackerPeers::dropBogons(*peers);
    }
    return true;
}

int UdpTrackerProto::timeoutSecs(int attempt) {
    static const int schedule[] = {15, 30, 60};
    if (attempt < 0) attempt = 0;
    if (attempt > 2) attempt = 2;
    return schedule[attempt];
}

namespace {
// Deterministic per-instance transaction id: seeded counter, no RNG (matches the
// torrent subsystem's no-RNG rule). Only needs to correlate request<->response.
} // namespace

UdpTrackerClient::UdpTrackerClient(const QUrl& tracker, quint32 rngSeed, QObject* parent)
    : ITrackerClient(parent), m_tracker(tracker), m_key(rngSeed ^ 0x9E3779B9u) {}

// One announce = one connect->announce exchange over a fresh datagram flow.
// State for this exchange lives in a small heap struct kept alive by lambdas
// captured on the socket; it self-destructs on success/failure/give-up (see
// cleanup(), below) — or, if `this` is destroyed first while it's still in
// flight, by the ~UdpTrackerClient() sweep over m_liveExchanges.
struct UdpTrackerClient::Exchange {
    UdpTrackerClient* self;
    QByteArray infoHash, peerId;
    quint16 port; qint64 downloaded, left; TrackerEvent ev;
    QUdpSocket* sock = nullptr;
    QTimer* timer = nullptr;
    quint32 txId = 0;
    quint64 connId = 0;
    int attempt = 0;
    enum { Connecting, Announcing } phase = Connecting;
};

UdpTrackerClient::~UdpTrackerClient() {
    qDeleteAll(m_liveExchanges);
    m_liveExchanges.clear();
}

void UdpTrackerClient::announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                                qint64 downloaded, qint64 left, TrackerEvent ev) {
    const bool fast = qEnvironmentVariableIsSet("ORBIT_UDP_FAST_TIMEOUT");
    auto* ex = new Exchange{this, infoHash, peerId, port, downloaded, left, ev};
    m_liveExchanges.insert(ex);
    ex->sock = new QUdpSocket(this);
    ex->timer = new QTimer(this);
    ex->timer->setSingleShot(true);
    ex->txId = ++m_txCounter ^ m_key; // deterministic, per-exchange

    const QString host = m_tracker.host();
    const quint16 tport = quint16(m_tracker.port(80));

    // Teardown must be safe against re-entry: a second datagram can already be
    // queued on ex->sock when we decide to finish (success/failure/give-up).
    // deleteLater() only destroys sock/timer on the next event-loop pass, so we
    // MUST synchronously disconnect their signals from `this` before `delete ex`
    // — otherwise a re-fired readyRead (or a timer tick that raced in) would
    // invoke a lambda that captured `ex` after it's already freed (use-after-free).
    auto cleanup = [ex]() {
        QObject::disconnect(ex->sock, nullptr, ex->self, nullptr);
        ex->timer->stop();
        QObject::disconnect(ex->timer, nullptr, ex->self, nullptr);
        ex->sock->deleteLater();
        ex->timer->deleteLater();
        ex->self->m_liveExchanges.remove(ex); // erase before delete: no double-free from the dtor sweep
        delete ex;
    };
    auto timeoutMs = [fast](int attempt) -> int {
        return fast ? (200 + 100 * attempt) : UdpTrackerProto::timeoutSecs(attempt) * 1000;
    };

    auto sendConnect = [=]() {
        ex->phase = Exchange::Connecting;
        ex->sock->writeDatagram(UdpTrackerProto::buildConnectRequest(ex->txId),
                                QHostAddress(host), tport);
        ex->timer->start(timeoutMs(ex->attempt));
    };
    auto sendAnnounce = [=]() {
        ex->phase = Exchange::Announcing;
        ex->sock->writeDatagram(
            UdpTrackerProto::buildAnnounceRequest(ex->connId, ex->txId, ex->infoHash, ex->peerId,
                                                  ex->downloaded, ex->left, 0, ex->ev, m_key, ex->port),
            QHostAddress(host), tport);
        ex->timer->start(timeoutMs(ex->attempt));
    };

    connect(ex->timer, &QTimer::timeout, this, [=]() {
        if (++ex->attempt > 2) { // BEP 15 give-up after the capped retries
            emit announceFailed(QStringLiteral("udp tracker timed out: %1").arg(m_tracker.toString()));
            cleanup();
            return;
        }
        ex->txId = ++m_txCounter ^ m_key;
        if (ex->phase == Exchange::Connecting) sendConnect(); else sendAnnounce();
    });

    connect(ex->sock, &QUdpSocket::readyRead, this, [=]() {
        while (ex->sock->hasPendingDatagrams()) {
            const QByteArray dg = ex->sock->receiveDatagram().data();
            if (ex->phase == Exchange::Connecting) {
                quint64 cid = 0;
                if (!UdpTrackerProto::parseConnectResponse(dg, ex->txId, &cid)) continue; // ignore junk
                ex->timer->stop();
                ex->connId = cid;
                ex->attempt = 0;
                ex->txId = ++m_txCounter ^ m_key;
                sendAnnounce();
            } else {
                int interval = 0; QVector<PeerAddress> peers; QString err; bool isTrackerError = false;
                if (!UdpTrackerProto::parseAnnounceResponse(dg, ex->txId, &interval, &peers, &err,
                                                            &isTrackerError)) {
                    // Only a genuine tracker error-action (action==3, our txId) is
                    // fatal. Anything else — txId mismatch, a short/garbage stray
                    // datagram, a malformed reply not addressed to us — is junk on
                    // the shared socket: ignore it and keep waiting for the real
                    // reply or the retry timer.
                    if (!isTrackerError) continue;
                    ex->timer->stop();
                    emit announceFailed(err);
                    cleanup();
                    return;
                }
                ex->timer->stop();
                emit peersReceived(peers, interval, 0); // UDP has no "min interval" field
                cleanup();
                return;
            }
        }
    });

    // Bind to any local port so we can receive replies, then kick off connect.
    if (QHostAddress(host).isNull()) {
        emit announceFailed(QStringLiteral("udp tracker host is not a numeric address (DNS is a follow-up): %1").arg(host));
        cleanup();
        return;
    }
    if (!ex->sock->bind(QHostAddress::AnyIPv4, 0)) {
        emit announceFailed(QStringLiteral("udp bind failed"));
        cleanup();
        return;
    }
    sendConnect();
}

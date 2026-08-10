#pragma once

#include "torrent/ITrackerClient.h"
#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QSet>
#include <QUrl>
#include <QVector>

// Pure BEP 15 packet codecs + retry schedule. No sockets: fully unit-testable.
namespace UdpTrackerProto {

constexpr quint64 kProtocolId = 0x41727101980ULL;

quint32     eventCode(TrackerEvent ev); // none=0, completed=1, started=2, stopped=3
QByteArray  buildConnectRequest(quint32 txId);
bool        parseConnectResponse(const QByteArray& dg, quint32 expectTxId, quint64* connId);
QByteArray  buildAnnounceRequest(quint64 connId, quint32 txId, const QByteArray& infoHash,
                                 const QByteArray& peerId, qint64 downloaded, qint64 left,
                                 qint64 uploaded, TrackerEvent ev, quint32 key, quint16 port);
// On a genuine tracker error (action==3 AND txId matches expectTxId) sets
// *errorMsg, sets *isTrackerError = true, and returns false: the caller should
// treat this as fatal for the exchange. On any other rejection (txId mismatch,
// short datagram, malformed response) sets *errorMsg, sets *isTrackerError =
// false, and returns false: this is "not for us" / junk and the caller should
// keep waiting rather than tear down the exchange. On a valid announce reply
// fills *intervalSecs and *peers (bogon-filtered) and returns true.
bool        parseAnnounceResponse(const QByteArray& dg, quint32 expectTxId, int* intervalSecs,
                                  QVector<PeerAddress>* peers, QString* errorMsg, bool* isTrackerError);
int         timeoutSecs(int attempt); // 15, 30, 60 (attempt 0..2); caller gives up after 2

} // namespace UdpTrackerProto

// UDP BEP 15 tracker client — declared here, implemented in Task 5.
class UdpTrackerClient : public ITrackerClient {
    Q_OBJECT
public:
    UdpTrackerClient(const QUrl& tracker, quint32 rngSeed, QObject* parent = nullptr);
    ~UdpTrackerClient() override;

    QUrl trackerUrl() const override { return m_tracker; }
    void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                  qint64 downloaded, qint64 left, TrackerEvent ev) override;

private:
    struct Exchange; // defined in the .cpp; one heap instance per in-flight announce()

    QUrl        m_tracker;
    quint32     m_key = 0;
    quint32     m_txCounter = 0;

    // Every Exchange currently in flight, so the dtor can free any that are
    // still pending if this client is destroyed mid-announce (e.g. the task
    // is paused/canceled while waiting out a dead tracker's ~105s give-up).
    // Without this sweep, cleanup() — the only other place ex is freed — never
    // runs: destroying `this` also destroys the QObject-parented sock/timer,
    // which auto-disconnects the this-context lambdas that would call it.
    QSet<Exchange*> m_liveExchanges;
};

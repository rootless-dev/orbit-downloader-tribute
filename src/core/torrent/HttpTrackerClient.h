#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;

// One peer address as reported by a tracker (compact or dictionary form).
struct PeerAddress {
    QString host;
    quint16 port = 0;
};

// BEP 3 announce event, sent as the tracker's "event" query parameter.
// None omits the parameter entirely (a plain periodic re-announce).
enum class TrackerEvent { None, Started, Stopped, Completed };

// TrackerProto holds the pure, network-free helpers that build the announce
// request and parse the tracker's bencoded response. Kept free of I/O so it
// can be unit-tested in isolation from HttpTrackerClient's QNetworkAccessManager
// plumbing.
namespace TrackerProto {

// Builds the full announce URL: `base` with a query string appended containing
// info_hash, peer_id, port, uploaded=0, downloaded, left, compact=1 and
// (unless ev is TrackerEvent::None) event. infoHash and peerId are percent-encoded
// byte-wise (every byte encoded, no "safe" exceptions) so the raw bytes survive
// the round trip regardless of their content.
QUrl buildAnnounceUrl(const QUrl& base, const QByteArray& infoHash, const QByteArray& peerId,
                      quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev);

// Bencode-decodes `body` as a tracker announce response. On a "failure reason"
// response, sets *failure and returns false. Otherwise fills *peers (compact
// 6-byte-per-peer or dictionary-of-peers form, both supported), *interval, and
// *minInterval (the optional BEP 3 "min interval" key — the tracker's floor on
// re-announce frequency; 0 if the key is absent), and returns true. Any
// pointer argument may be null.
bool parseResponse(const QByteArray& body, QVector<PeerAddress>* peers, int* interval, int* minInterval,
                   QString* failure);

} // namespace TrackerProto

// HttpTrackerClient issues a BEP 3 HTTP(S) tracker announce and reports the
// result asynchronously. A single instance can be reused for repeated
// announces; it does not own the QNetworkAccessManager it is given.
class HttpTrackerClient : public QObject {
    Q_OBJECT
public:
    explicit HttpTrackerClient(QNetworkAccessManager* nam, QObject* parent = nullptr);

    // Fires off an HTTP GET to the tracker's announce URL. Emits peersReceived
    // on a well-formed response, announceFailed otherwise (network error,
    // a "failure reason" response, or an unsupported udp:// scheme tracker
    // — the latter is reported synchronously, before any request is sent).
    void announce(const QUrl& tracker, const QByteArray& infoHash, const QByteArray& peerId,
                  quint16 port, qint64 downloaded, qint64 left, TrackerEvent ev);

signals:
    // minIntervalSecs is the tracker's BEP 3 "min interval" (0 if the tracker
    // didn't send one) — the floor TorrentTask's adaptive re-announce cadence
    // must never announce faster than, so it can't get a well-behaved client
    // rate-limited/banned by a strict tracker.
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);

private:
    QNetworkAccessManager* m_nam;
};

Q_DECLARE_METATYPE(PeerAddress)
Q_DECLARE_METATYPE(QVector<PeerAddress>)

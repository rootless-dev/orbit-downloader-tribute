#pragma once

#include "torrent/Bitfield.h"
#include "torrent/HttpTrackerClient.h" // PeerAddress
#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpSocket;
class QTimer;
class RateLimiter;

// A single outbound BitTorrent peer connection (BEP 3), download-only.
//
// We never serve pieces: PeerConnection advertises no bitfield, never sends
// unchoke/piece, and ignores any request/interested/cancel the peer sends
// us (am_choking is implicitly always true — we simply don't act on them).
// The only messages we ever send are our handshake, interested, and
// request.
//
// Wire I/O flows over a QTcpSocket. Incoming bytes accumulate in a member
// buffer: the first 68 bytes are consumed as the peer's handshake (its
// info_hash is validated against ours), then PeerWire::parseMessage drains
// complete frames in a loop, tolerating a message split across multiple
// readyRead deliveries. RateLimiter (optional, may be null) throttles reads
// exactly like SegmentWorker/FtpSegmentWorker: setReadBufferSize caps how
// far the OS socket buffer can grow (backpressure via TCP), and take()
// bounds how many buffered bytes we actually consume per readyRead,
// re-arming a short drain timer when tokens ran out mid-buffer.
class PeerConnection : public QObject {
    Q_OBJECT
public:
    PeerConnection(const PeerAddress& addr, const QByteArray& infoHash, const QByteArray& peerId,
                   int pieceCount, RateLimiter* limiter, QObject* parent = nullptr);

    // Metadata-mode ctor (BEP 9/10): no pieceCount because there's no
    // bitfield/piece logic to run — this connection only does the BT
    // handshake and the BEP 10 extended handshake (and, once Task 12 lands,
    // ut_metadata requests). Used to fetch the .torrent's info dict from
    // peers when we only have a magnet link.
    PeerConnection(const PeerAddress& addr, const QByteArray& infoHash, const QByteArray& peerId,
                   RateLimiter* limiter, QObject* parent = nullptr);

    void connectToPeer();
    void sendInterested();
    void sendRequest(int piece, qint64 begin, qint64 length);

    const Bitfield& peerBitfield() const;
    bool amUnchoked() const;
    const PeerAddress& address() const { return m_addr; }
    // "<host>:<port>" — used in log lines/GUI so a peer is identifiable
    // without exposing the whole PeerAddress struct.
    QString label() const { return m_addr.host + QLatin1Char(':') + QString::number(m_addr.port); }

signals:
    void handshakeOk();
    // Human-readable, low-volume wire diagnostics (peer reserved bytes, which
    // availability message the peer used, first sighting of an unhandled
    // message id). Forwarded to the per-download log at Debug level. Exists to
    // ground-truth what real peers send on the wire.
    void peerLog(QString line);
    void bitfieldReceived();
    void haveReceived(int piece);
    void unchoked();
    void choked();
    void blockReceived(int piece, qint64 begin, QByteArray data);
    // BEP 10: fired once the peer's extended handshake (ext id 20/0) has
    // been parsed. utMetadataId is the peer's chosen id for the ut_metadata
    // extension (0 if the peer doesn't support/advertise it); metadataSize
    // is the info-dict size in bytes from the "metadata_size" key (0 if
    // absent, e.g. the peer doesn't have the full metadata yet either).
    void extendedHandshake(int utMetadataId, int metadataSize);
    // BEP 9: fired once every piece of the info dict has been requested,
    // received, and its SHA-1 verified against m_infoHash. infoDict holds
    // the raw, already-verified bytes (exactly metadataSize long).
    void metadataComplete(QByteArray infoDict);
    // BEP 9: fired instead of metadataComplete if the peer rejects a piece
    // request (msg_type 2), sends a malformed/out-of-range data message, or
    // the fully-assembled metadata's SHA-1 doesn't match m_infoHash.
    void metadataFailed();
    void disconnected(QString reason);

private:
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onSocketDisconnected();
    void scheduleDrain();
    void processBuffer();
    void handleMessage(int id, const QByteArray& payload);
    void failAndDisconnect(const QString& reason);

    // BEP 9 ut_metadata fetch (metadata mode only).
    void startMetadataFetch(int peerUtMetadataId, int metadataSize);
    void handleUtMetadataMessage(const QByteArray& extPayload);
    void finishMetadata();
    void failMetadata();

    QTcpSocket*  m_sock = nullptr;
    RateLimiter* m_limiter = nullptr;
    QTimer*      m_drainTimer = nullptr;

    PeerAddress m_addr;
    QByteArray  m_infoHash;
    QByteArray  m_peerId;
    int         m_pieceCount = 0;
    // Metadata-only mode (BEP 9/10 fetch): skip bitfield/piece handling
    // entirely and send the extended handshake right after the BT one.
    bool        m_metadataMode = false;

    QByteArray m_buf;
    QSet<int>  m_loggedUnhandled; // ids already surfaced via peerLog (dedup spam)
    bool       m_handshakeDone = false;
    bool       m_unchoked = false;
    bool       m_dead = false;
    Bitfield   m_peerBitfield;

    // BEP 9 ut_metadata fetch state (metadata mode only).
    int        m_peerUtMetadataId = 0;  // peer's chosen ext id; requests are addressed to it
    int        m_metadataSize = 0;
    int        m_metadataPieceCount = 0;
    QByteArray m_metadataBuf;
    QSet<int>  m_metadataPiecesReceived;
    bool       m_metadataRequested = false; // guards against a duplicate/repeated ext handshake
    bool       m_metadataDone = false;      // guards against emitting complete/failed twice
};

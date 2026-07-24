#pragma once

#include "torrent/Bitfield.h"
#include "torrent/HttpTrackerClient.h" // PeerAddress
#include <QByteArray>
#include <QObject>
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
    void bitfieldReceived();
    void haveReceived(int piece);
    void unchoked();
    void choked();
    void blockReceived(int piece, qint64 begin, QByteArray data);
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

    QTcpSocket*  m_sock = nullptr;
    RateLimiter* m_limiter = nullptr;
    QTimer*      m_drainTimer = nullptr;

    PeerAddress m_addr;
    QByteArray  m_infoHash;
    QByteArray  m_peerId;
    int         m_pieceCount = 0;

    QByteArray m_buf;
    bool       m_handshakeDone = false;
    bool       m_unchoked = false;
    bool       m_dead = false;
    Bitfield   m_peerBitfield;
};

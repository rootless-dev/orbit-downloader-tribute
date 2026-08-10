#pragma once

#include "torrent/TrackerTypes.h"

#include <QObject>
#include <QVector>

class QUdpSocket;

// Minimal BEP 15 UDP tracker for offline tests. Answers connect (a fixed
// connection id) and announce (returns the configured peers + interval).
// Mirrors TestSeeder / TestFtpServer: in-process, event-loop, no threads.
class TestUdpTracker : public QObject {
    Q_OBJECT
public:
    explicit TestUdpTracker(QObject* parent = nullptr);
    quint16 port() const;
    void    setPeers(const QVector<PeerAddress>& peers) { m_peers = peers; }
    void    setInterval(int secs) { m_interval = secs; }
    // Test-only opt-ins for exercising the client's teardown/junk handling.
    // When set, sends the real announce reply datagram TWICE back-to-back
    // (same txId) to simulate a duplicate/second-queued-datagram race.
    void    setDuplicateAnnounceReply(bool on) { m_duplicateAnnounceReply = on; }
    // When set, sends a stray 4-byte garbage datagram immediately BEFORE the
    // real announce reply, so the client sees junk while still awaiting the
    // real reply for this exchange.
    void    setSendJunkDatagram(bool on) { m_sendJunkDatagram = on; }

private:
    void onReadyRead();
    QUdpSocket*           m_sock = nullptr;
    QVector<PeerAddress>  m_peers;
    int                   m_interval = 1800;
    bool                  m_duplicateAnnounceReply = false;
    bool                  m_sendJunkDatagram = false;
    static constexpr quint64 kConnId = 0x0123456789ABCDEFULL;
};

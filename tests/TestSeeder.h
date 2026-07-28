#pragma once

#include <QByteArray>
#include <QObject>
#include <QTcpServer>

// In-process, test-only BitTorrent SEEDER (BEP 3) over QTcpSocket. Serves a
// known torrent for offline end-to-end tests of the download engine — the
// deliberate opposite of PeerConnection (download-only): TestSeeder never
// requests anything, it advertises a full bitfield, unchokes immediately,
// and answers every `request` with the matching `piece`. Not shipped in the
// app; lives in tests/ alongside TestServer/TestFtpServer.
class TestSeeder : public QObject {
    Q_OBJECT
public:
    // How the seeder advertises the pieces it holds after the handshake.
    // NormalBitfield mirrors real seeders on the wire; the other two exist
    // only to reproduce the two field-bug hypotheses offline.
    enum class Advertise {
        NormalBitfield,     // one write: full <bitfield> frame (default)
        HaveAll,            // BEP 6 <have_all> (id 14), NO bitfield frame
        FragmentedBitfield, // full bitfield, but split across two readyRead
                            // deliveries (models real-network TCP segmentation
                            // of a >MTU bitfield, which loopback never does)
    };

    // Serves `data` split into `pieceLength` pieces for `infoHash`. Listens
    // on 127.0.0.1:port().
    TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength,
               QObject* parent = nullptr);
    TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength,
               Advertise advertise, QObject* parent = nullptr);

    quint16 port() const { return m_server.serverPort(); }

private:
    struct Session;
    void onNewConnection();

    QTcpServer m_server;
    QByteArray m_infoHash;
    QByteArray m_data;
    qint64     m_pieceLength;
    int        m_pieceCount;
    Advertise  m_advertise;
};

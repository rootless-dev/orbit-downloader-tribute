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
    // Serves `data` split into `pieceLength` pieces for `infoHash`. Listens
    // on 127.0.0.1:port().
    TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength,
               QObject* parent = nullptr);

    quint16 port() const { return m_server.serverPort(); }

private:
    struct Session;
    void onNewConnection();

    QTcpServer m_server;
    QByteArray m_infoHash;
    QByteArray m_data;
    qint64     m_pieceLength;
    int        m_pieceCount;
};

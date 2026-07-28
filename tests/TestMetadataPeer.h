#pragma once

#include <QByteArray>
#include <QObject>
#include <QTcpServer>

// In-process, test-only BitTorrent peer that serves the BEP 9 ut_metadata
// extension: completes the BT handshake, sends a BEP 10 extended handshake
// advertising "ut_metadata" (its own chosen extension id) and
// "metadata_size", then answers each incoming `{msg_type:0, piece:N}`
// request with a `{msg_type:1, piece:N, total_size:S}` "data" message
// immediately followed by the raw 16 KiB (or shorter final) block of the
// info dict it was constructed with. Used to test PeerConnection's
// metadata-mode fetch (Task 12) without a real peer. Modeled on TestSeeder.
class TestMetadataPeer : public QObject {
    Q_OBJECT
public:
    // Serves `infoDict` (the raw bencoded info dict bytes) to any client
    // that connects and completes the handshake with `infoHash`. Listens on
    // 127.0.0.1:port(). `infoDict` need not actually hash to `infoHash` —
    // tests use that mismatch to model a peer serving tampered metadata.
    //
    // `advertisedMetadataSize`, if >= 0, overrides the metadata_size sent in
    // the extended handshake independently of infoDict's real size — lets a
    // test model a peer that lies about a huge metadata_size without
    // actually having to construct that many bytes.
    TestMetadataPeer(const QByteArray& infoHash, const QByteArray& infoDict,
                      QObject* parent = nullptr, int advertisedMetadataSize = -1);

    quint16 port() const { return m_server.serverPort(); }

private:
    struct Session;
    void onNewConnection();

    QTcpServer m_server;
    QByteArray m_infoHash;
    QByteArray m_infoDict;
    int m_advertisedMetadataSize;
};

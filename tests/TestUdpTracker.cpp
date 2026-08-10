#include "TestUdpTracker.h"

#include <QHostAddress>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>

TestUdpTracker::TestUdpTracker(QObject* parent) : QObject(parent), m_sock(new QUdpSocket(this)) {
    m_sock->bind(QHostAddress::LocalHost, 0);
    connect(m_sock, &QUdpSocket::readyRead, this, &TestUdpTracker::onReadyRead);
}

quint16 TestUdpTracker::port() const { return m_sock->localPort(); }

void TestUdpTracker::onReadyRead() {
    while (m_sock->hasPendingDatagrams()) {
        QNetworkDatagram in = m_sock->receiveDatagram();
        const QByteArray req = in.data();
        if (req.size() < 16) continue;
        const auto* p = reinterpret_cast<const uchar*>(req.constData());
        const quint32 action = qFromBigEndian<quint32>(p + 8);
        const quint32 txId = qFromBigEndian<quint32>(p + 12);

        QByteArray resp;
        if (action == 0u) { // connect
            resp.resize(16);
            auto* q = reinterpret_cast<uchar*>(resp.data());
            qToBigEndian<quint32>(0u, q);
            qToBigEndian<quint32>(txId, q + 4);
            qToBigEndian<quint64>(kConnId, q + 8);
        } else if (action == 1u) { // announce
            if (m_sendJunkDatagram) {
                // Stray short/garbage datagram, sent before the real reply, to
                // verify the client ignores junk on the exchange's socket
                // instead of treating it as a fatal tracker error.
                m_sock->writeDatagram(QByteArray(4, char(0xFF)), in.senderAddress(), in.senderPort());
            }
            resp.resize(20);
            auto* q = reinterpret_cast<uchar*>(resp.data());
            qToBigEndian<quint32>(1u, q);
            qToBigEndian<quint32>(txId, q + 4);
            qToBigEndian<quint32>(quint32(m_interval), q + 8);
            qToBigEndian<quint32>(0u, q + 12); // leechers
            qToBigEndian<quint32>(quint32(m_peers.size()), q + 16); // seeders
            for (const PeerAddress& peer : m_peers) {
                QByteArray rec(6, char(0));
                const auto parts = peer.host.split('.');
                for (int i = 0; i < 4 && i < parts.size(); ++i) rec[i] = char(parts[i].toInt());
                rec[4] = char((peer.port >> 8) & 0xFF);
                rec[5] = char(peer.port & 0xFF);
                resp += rec;
            }
        } else {
            continue;
        }
        m_sock->writeDatagram(resp, in.senderAddress(), in.senderPort());
        if (action == 1u && m_duplicateAnnounceReply) {
            // Send the exact same reply a second time, back-to-back, to
            // simulate a second datagram already queued on the client's
            // socket at the moment the exchange resolves.
            m_sock->writeDatagram(resp, in.senderAddress(), in.senderPort());
        }
    }
}

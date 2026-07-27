#pragma once

#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QVector>

// Pure peer-list parsing shared by the HTTP and UDP tracker clients. No Qt
// networking, no Bencode: it turns raw compact records into PeerAddress values
// and filters addresses that can never be a real peer.
namespace TrackerPeers {

// Compact IPv4 (BEP 23): 6 bytes per record — 4-byte big-endian IPv4 + 2-byte
// big-endian port. A trailing partial (<6 byte) record is ignored, not an error.
QVector<PeerAddress> fromCompactV4(const QByteArray& raw);

// Compact IPv6 (BEP 7): 18 bytes per record — 16-byte IPv6 + 2-byte big-endian
// port. The host is rendered as a bracketless literal (QTcpSocket wants it plain).
QVector<PeerAddress> fromCompactV6(const QByteArray& raw);

// True for addresses that can never be a real peer: 0.0.0.0/8, loopback
// (127/8, ::1), multicast (224/4, ff00::/8), 255.255.255.255, unspecified ::,
// or port 0. Deliberately does NOT reject RFC1918/link-local — LAN peers are
// legitimate, and an unreachable one merely fails a single TCP connect.
//
// Test-only seam: when the environment variable ORBIT_ALLOW_LOOPBACK_PEERS is
// set, loopback addresses (127.0.0.0/8 and ::1) are NOT treated as bogon and
// are kept. This exists solely so offline, in-process E2E tests can announce
// a seeder bound to 127.0.0.1 through a real tracker round trip and have the
// resulting peer survive to AnnounceController. Every other always-invalid
// category (0.0.0.0/8, unspecified ::, multicast, broadcast, port 0) is
// unaffected by the env var and is always dropped. Production behavior is
// unchanged when the var is unset: loopback peers are always bogon, exactly
// as before (see UdpTrackerClient's ORBIT_UDP_FAST_TIMEOUT for the same
// test-seam-via-env-var pattern already used in this codebase).
bool isBogon(const PeerAddress& p);

// Removes every isBogon() entry from v in place, preserving order.
void dropBogons(QVector<PeerAddress>& v);

} // namespace TrackerPeers

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include "torrent/Bencode.h"

// KRPC (DHT wire protocol) message, decoded from / encoded to bencode.
// See BEP 5: top-level dict has "t" (transaction id), "y" ("q"/"r"/"e"),
// plus "q"+"a" for queries, "r" for responses, "e" = [code, msg] for errors.
struct KrpcMessage {
    enum Kind { Query, Response, Error, Invalid };
    Kind kind = Invalid;
    QByteArray tid;                       // "t"
    QString method;                       // query name ("ping"/"find_node"/"get_peers"/"announce_peer")
    QMap<QByteArray, BencodeValue> args;   // "a" (query) or "r" (response)
    int errorCode = 0;
    QString errorMsg;
};

namespace KrpcCodec {
QByteArray encodeQuery(const QByteArray& tid, const QString& method,
                       const QMap<QByteArray, BencodeValue>& args);
QByteArray encodeResponse(const QByteArray& tid, const QMap<QByteArray, BencodeValue>& r);
QByteArray encodeError(const QByteArray& tid, int code, const QString& msg);

// Returns a KrpcMessage with kind==Invalid on ANY malformed input (empty,
// truncated, wrong types, missing keys). Never throws, never crashes.
KrpcMessage decode(const QByteArray& datagram);
}

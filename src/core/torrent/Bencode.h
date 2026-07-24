#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

// Bencode (BitTorrent) codec.
//
// BencodeValue records, for every parsed node, the raw byte span
// [rawBegin, rawEnd) it occupied in the ORIGINAL input buffer. This is
// essential for computing the info-hash later: the SHA-1 must be taken
// over the raw bytes of the "info" dict exactly as they appear in the
// .torrent file, never a re-encoded copy (canonicalization could differ
// from the original in subtle ways, e.g. key ordering already provided
// by a non-conforming producer, and would produce the wrong hash).
class BencodeValue {
public:
    enum class Type { Int, Bytes, List, Dict };

    BencodeValue() = default;

    Type type() const { return m_type; }
    qint64 toInt() const { return m_int; }
    QByteArray toBytes() const { return m_bytes; }
    const QList<BencodeValue>& toList() const { return m_list; }
    const QMap<QByteArray, BencodeValue>& toDict() const { return m_dict; } // key-sorted

    bool contains(const QByteArray& key) const { return m_dict.contains(key); }
    const BencodeValue& operator[](const QByteArray& key) const {
        auto it = m_dict.find(key);
        if (it == m_dict.end()) {
            static const BencodeValue empty;
            return empty;
        }
        return it.value();
    }

    // Raw source span of THIS value within the parsed buffer (for info-hash):
    int rawBegin() const { return m_rawBegin; }
    int rawEnd() const { return m_rawEnd; }

    // --- Construction helpers (used by the decoder / callers building values to encode) ---
    static BencodeValue makeInt(qint64 v) {
        BencodeValue r;
        r.m_type = Type::Int;
        r.m_int = v;
        return r;
    }
    static BencodeValue makeBytes(QByteArray v) {
        BencodeValue r;
        r.m_type = Type::Bytes;
        r.m_bytes = std::move(v);
        return r;
    }
    static BencodeValue makeList(QList<BencodeValue> v) {
        BencodeValue r;
        r.m_type = Type::List;
        r.m_list = std::move(v);
        return r;
    }
    static BencodeValue makeDict(QMap<QByteArray, BencodeValue> v) {
        BencodeValue r;
        r.m_type = Type::Dict;
        r.m_dict = std::move(v);
        return r;
    }
    void setRawSpan(int begin, int end) {
        m_rawBegin = begin;
        m_rawEnd = end;
    }

private:
    Type m_type = Type::Int;
    qint64 m_int = 0;
    QByteArray m_bytes;
    QList<BencodeValue> m_list;
    QMap<QByteArray, BencodeValue> m_dict;
    int m_rawBegin = 0;
    int m_rawEnd = 0;
};

namespace Bencode {
// ok=false on malformed input; err set to a human-readable message (includes the byte offset).
BencodeValue decode(const QByteArray& in, bool* ok, QString* err = nullptr);
QByteArray encode(const BencodeValue& v);
}

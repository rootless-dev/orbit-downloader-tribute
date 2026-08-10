#include "torrent/Bencode.h"

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Hard cap on list/dict nesting depth. Far beyond any legitimate torrent,
// KRPC, or ut_metadata structure — exists solely to stop a maliciously deep
// bencode payload (e.g. "l" repeated hundreds of thousands of times) from
// blowing the native stack via unbounded recursion.
constexpr int kMaxNestingDepth = 200;

// Recursive-descent decoder over the input buffer. Tracks a cursor position
// and records, for every value parsed, its raw [begin, end) span so callers
// can later slice the exact original bytes (needed for info-hash hashing).
class Decoder {
public:
    explicit Decoder(const QByteArray& in) : m_in(in) {}

    BencodeValue parseValue(int depth = 0) {
        if (m_failed) return {};
        if (atEnd()) {
            fail("unexpected end of input");
            return {};
        }
        const char c = m_in[m_pos];
        if (c == 'i') return parseInt();
        if (c == 'l') return parseList(depth);
        if (c == 'd') return parseDict(depth);
        if (isDigit(c)) return parseBytes();
        fail(QString("unexpected character '%1'").arg(QChar::fromLatin1(c)));
        return {};
    }

    bool failed() const { return m_failed; }
    int pos() const { return m_pos; }
    int errorPos() const { return m_errorPos; }
    QString errorMessage() const { return m_errMsg; }

private:
    const QByteArray& m_in;
    int m_pos = 0;
    bool m_failed = false;
    QString m_errMsg;
    int m_errorPos = -1;

    bool atEnd() const { return m_pos >= m_in.size(); }

    void fail(const QString& msg) {
        if (m_failed) return; // keep the first error
        m_failed = true;
        m_errMsg = msg;
        m_errorPos = m_pos;
    }

    // i<digits>e ; no leading zero (except "i0e"); no "-0"
    BencodeValue parseInt() {
        const int begin = m_pos;
        m_pos++; // skip 'i'
        if (atEnd()) { fail("truncated integer"); return {}; }
        bool neg = false;
        if (m_in[m_pos] == '-') {
            neg = true;
            m_pos++;
        }
        if (atEnd() || !isDigit(m_in[m_pos])) { fail("invalid integer"); return {}; }
        const int digitsStart = m_pos;
        while (!atEnd() && isDigit(m_in[m_pos])) m_pos++;
        if (atEnd() || m_in[m_pos] != 'e') { fail("unterminated integer"); return {}; }
        const QByteArray digits = m_in.mid(digitsStart, m_pos - digitsStart);
        if (digits.size() > 1 && digits[0] == '0') { fail("integer has leading zero"); return {}; }
        if (neg && digits == "0") { fail("negative zero is not allowed"); return {}; }
        bool convOk = false;
        qint64 value = digits.toLongLong(&convOk);
        if (!convOk) { fail("integer out of range"); return {}; }
        if (neg) value = -value;
        m_pos++; // skip 'e'
        BencodeValue v = BencodeValue::makeInt(value);
        v.setRawSpan(begin, m_pos);
        return v;
    }

    // <len>:<bytes>
    BencodeValue parseBytes() {
        const int begin = m_pos;
        const int lenStart = m_pos;
        while (!atEnd() && isDigit(m_in[m_pos])) m_pos++;
        if (atEnd() || m_in[m_pos] != ':') { fail("expected ':' in byte string length"); return {}; }
        const QByteArray lenStr = m_in.mid(lenStart, m_pos - lenStart);
        if (lenStr.size() > 1 && lenStr[0] == '0') { fail("byte string length has leading zero"); return {}; }
        bool convOk = false;
        const qint64 len = lenStr.toLongLong(&convOk);
        if (!convOk || len < 0) { fail("invalid byte string length"); return {}; }
        m_pos++; // skip ':'
        if (len > m_in.size() - m_pos) { fail("truncated byte string"); return {}; }
        const QByteArray bytes = m_in.mid(m_pos, static_cast<int>(len));
        m_pos += static_cast<int>(len);
        BencodeValue v = BencodeValue::makeBytes(bytes);
        v.setRawSpan(begin, m_pos);
        return v;
    }

    // l<values>e
    BencodeValue parseList(int depth) {
        const int begin = m_pos;
        m_pos++; // skip 'l'
        if (depth + 1 > kMaxNestingDepth) { fail("nesting depth exceeds limit"); return {}; }
        QList<BencodeValue> items;
        while (true) {
            if (atEnd()) { fail("truncated list"); return {}; }
            if (m_in[m_pos] == 'e') {
                m_pos++;
                break;
            }
            BencodeValue item = parseValue(depth + 1);
            if (m_failed) return {};
            items.append(std::move(item));
        }
        BencodeValue v = BencodeValue::makeList(std::move(items));
        v.setRawSpan(begin, m_pos);
        return v;
    }

    // d<key><value>...e ; keys must be byte strings
    BencodeValue parseDict(int depth) {
        const int begin = m_pos;
        m_pos++; // skip 'd'
        if (depth + 1 > kMaxNestingDepth) { fail("nesting depth exceeds limit"); return {}; }
        QMap<QByteArray, BencodeValue> dict;
        while (true) {
            if (atEnd()) { fail("truncated dict"); return {}; }
            if (m_in[m_pos] == 'e') {
                m_pos++;
                break;
            }
            if (!isDigit(m_in[m_pos])) { fail("dict key must be a byte string"); return {}; }
            BencodeValue key = parseBytes();
            if (m_failed) return {};
            BencodeValue value = parseValue(depth + 1);
            if (m_failed) return {};
            dict.insert(key.toBytes(), std::move(value));
        }
        BencodeValue v = BencodeValue::makeDict(std::move(dict));
        v.setRawSpan(begin, m_pos);
        return v;
    }
};

void encodeInto(const BencodeValue& v, QByteArray& out) {
    switch (v.type()) {
    case BencodeValue::Type::Int:
        out += 'i';
        out += QByteArray::number(v.toInt());
        out += 'e';
        break;
    case BencodeValue::Type::Bytes: {
        const QByteArray bytes = v.toBytes();
        out += QByteArray::number(static_cast<qint64>(bytes.size()));
        out += ':';
        out += bytes;
        break;
    }
    case BencodeValue::Type::List:
        out += 'l';
        for (const BencodeValue& item : v.toList()) encodeInto(item, out);
        out += 'e';
        break;
    case BencodeValue::Type::Dict:
        out += 'd';
        for (auto it = v.toDict().constBegin(); it != v.toDict().constEnd(); ++it) {
            encodeInto(BencodeValue::makeBytes(it.key()), out);
            encodeInto(it.value(), out);
        }
        out += 'e';
        break;
    }
}

} // namespace

namespace Bencode {

BencodeValue decode(const QByteArray& in, bool* ok, QString* err) {
    Decoder decoder(in);
    BencodeValue value = decoder.parseValue();
    if (!decoder.failed() && decoder.pos() != in.size()) {
        // Trailing bytes after a complete value are not part of a single
        // bencoded value; treat as malformed input.
        if (err) *err = QString("trailing data after value at byte %1").arg(decoder.pos());
        if (ok) *ok = false;
        return {};
    }
    if (ok) *ok = !decoder.failed();
    if (err) {
        *err = decoder.failed()
            ? QString("%1 at byte %2").arg(decoder.errorMessage()).arg(decoder.errorPos())
            : QString();
    }
    if (decoder.failed()) return {};
    return value;
}

QByteArray encode(const BencodeValue& v) {
    QByteArray out;
    encodeInto(v, out);
    return out;
}

} // namespace Bencode

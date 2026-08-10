#pragma once

#include <QByteArray>
#include <vector>
#include <cstdint>

// A bitset over torrent pieces (BEP 3 "bitfield" message payload).
//
// Bitfield is a pure value type: it tracks which pieces of a torrent are
// present and (de)serializes to the wire format used by the bitfield/have
// peer-wire messages. It performs no I/O.
//
// Wire format: MSB-first packing. Piece i lives in byte i/8, bit 7-(i%8)
// (i.e. piece 0 is the most-significant bit of byte 0). When pieceCount is
// not a multiple of 8, the trailing spare bits in the last byte are zero.
class Bitfield {
public:
    explicit Bitfield(int pieceCount = 0);

    int size() const;                  // piece count
    bool has(int i) const;
    void set(int i);
    int count() const;                 // pieces present
    bool isComplete() const;

    QByteArray toBytes() const;
    static Bitfield fromBytes(const QByteArray& b, int pieceCount);

private:
    int m_pieceCount = 0;
    std::vector<uint8_t> m_bits; // one entry per piece: 0 or 1
};

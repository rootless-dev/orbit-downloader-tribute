#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

// Parsed representation of a .torrent metainfo file (BEP 3).
//
// TorrentMetainfo::parse is a pure function: it decodes bencoded bytes,
// validates the fields required to drive a download (piece hashes, file
// layout, info-hash), and reports failure via `ok`/`err` rather than
// throwing. It performs no I/O.
struct FileEntry {
    QString path;
    qint64 length = 0;
    qint64 offset = 0; // cumulative byte offset into the logical (concatenated) file stream
};

struct TorrentMetainfo {
    QByteArray infoHash;              // 20-byte SHA-1 of the raw info dict bytes
    QString name;
    qint64 pieceLength = 0;
    QVector<QByteArray> pieceHashes;  // 20 bytes each
    qint64 totalLength = 0;
    QVector<FileEntry> files;         // >=1; offset is cumulative into the logical stream
    QUrl announce;
    QVector<QVector<QUrl>> announceList; // BEP 12 tiers; [[announce]] when the key is absent
    bool isMultiFile = false;

    // Parses `torrentBytes` as a bencoded .torrent file. On success returns a
    // fully populated TorrentMetainfo and sets *ok = true. On any malformed
    // or missing field, sets *ok = false (and *err, if provided, to a
    // human-readable message) and returns a default-constructed value.
    static TorrentMetainfo parse(const QByteArray& torrentBytes, bool* ok, QString* err = nullptr);

    // Hand-assembles a top-level .torrent dict around `infoDict`, splicing it
    // in VERBATIM (never re-encoded/re-decoded) as the "info" value -- so
    // parse()'s raw-span SHA-1 over the result hashes exactly `infoDict`'s
    // bytes, whatever keys it contains (including any this codebase doesn't
    // itself recognize/round-trip, e.g. BEP 27 "private", "source", per-file
    // "md5sum"...). `trackers` populate both "announce" (first tracker, or
    // empty if none) and a single-tier "announce-list" (magnet `tr=`
    // trackers are a flat, equal-priority set with no BEP 12 tiering info).
    // Pure/no I/O: callers decide what to do with the returned bytes (parse
    // them back via parse(), write them to disk as a cached .torrent, ...).
    static QByteArray wrapInfoDictAsTorrent(const QByteArray& infoDict, const QStringList& trackers);

    // Builds a full TorrentMetainfo from a RAW BEP 9 info dict (as recovered
    // by MetadataFetch from a magnet-link peer, already SHA-1-verified by
    // PeerConnection) plus the magnet's tracker list. Wraps `infoDict` via
    // wrapInfoDictAsTorrent() (verbatim, never re-encoded/re-decoded) and
    // parses the result, so the returned infoHash is guaranteed to equal
    // SHA1(infoDict) -- the same value the peer's metadata was already
    // verified against.
    static TorrentMetainfo fromInfoDict(const QByteArray& infoDict, const QStringList& trackers);
};

Q_DECLARE_METATYPE(TorrentMetainfo)

#pragma once

#include <QByteArray>
#include <QString>
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
    bool isMultiFile = false;

    // Parses `torrentBytes` as a bencoded .torrent file. On success returns a
    // fully populated TorrentMetainfo and sets *ok = true. On any malformed
    // or missing field, sets *ok = false (and *err, if provided, to a
    // human-readable message) and returns a default-constructed value.
    static TorrentMetainfo parse(const QByteArray& torrentBytes, bool* ok, QString* err = nullptr);
};

#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>

#include "torrent/TorrentMetainfo.h"

// One contiguous slice of a piece that lands inside a single logical file.
// `file` points into the PieceStore's own copy of the TorrentMetainfo's
// `files` vector, so it stays valid for the PieceStore's lifetime (but must
// not be retained beyond it).
struct WriteRegion {
    const FileEntry* file = nullptr;
    qint64 fileOffset = 0; // offset within `file` to read/write
    qint64 length = 0;     // number of bytes in this region
    qint64 bufOffset = 0;  // offset within the piece buffer this region maps to
};

// Maps torrent pieces onto the on-disk file layout, tracks which pieces are
// wanted given a file selection, and performs the disk I/O and SHA-1
// verification needed to write and validate downloaded pieces.
//
// PieceStore owns a copy of the TorrentMetainfo it is constructed with; the
// mapping methods (regionsFor/pieceSize/pieceIsWanted/wantedPieces) are pure
// and require no disk access. writePiece/verifyOnDisk perform I/O under
// `destDir`, creating parent directories and files lazily.
class PieceStore {
public:
    PieceStore(const TorrentMetainfo& m, const QString& destDir, const QSet<int>& selectedFiles);

    // Pure mapping: which file byte-ranges the absolute range
    // [piece*pieceLength+begin, +length) covers, split at file boundaries.
    QVector<WriteRegion> regionsFor(int piece, qint64 begin, qint64 length) const;

    // Size of `piece` in bytes; the last piece may be shorter than pieceLength.
    qint64 pieceSize(int piece) const;

    // Whether `piece`'s byte range overlaps any selected file.
    bool pieceIsWanted(int piece) const;

    // All piece indices for which pieceIsWanted() is true.
    QVector<int> wantedPieces() const;

    // Writes a full piece's bytes to disk, splitting across files as needed.
    // `data.size()` must equal pieceSize(piece). Creates parent directories
    // and files lazily; existing file contents outside the written range are
    // left untouched (files may be sparse). Returns false and sets *err on
    // failure.
    bool writePiece(int piece, const QByteArray& data, QString* err);

    // True iff SHA1(data) matches the piece's expected hash.
    bool verify(int piece, const QByteArray& data) const;

    // Reads the piece back from disk (across all its regions) and verifies
    // it. Returns false and sets *err on I/O failure (a hash mismatch is not
    // an I/O failure: it still returns false but *err is left describing the
    // read, or unset if the read succeeded and only the hash differed).
    bool verifyOnDisk(int piece, QString* err) const;

    // Root path for this download: <destDir>/<name>. For a single-file
    // torrent this is the file itself; for a multi-file torrent this is the
    // containing directory.
    QString destRootPath() const;

private:
    QString absolutePathFor(const FileEntry& file) const;

    TorrentMetainfo m_meta;
    QString m_destDir;
    QSet<int> m_selectedFiles;
};

#include "torrent/PieceStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <algorithm>

PieceStore::PieceStore(const TorrentMetainfo& m, const QString& destDir, const QSet<int>& selectedFiles)
    : m_meta(m), m_destDir(destDir), m_selectedFiles(selectedFiles) {}

QVector<WriteRegion> PieceStore::regionsFor(int piece, qint64 begin, qint64 length) const {
    QVector<WriteRegion> out;
    const qint64 start = qint64(piece) * m_meta.pieceLength + begin;
    const qint64 end = start + length;

    for (const FileEntry& file : m_meta.files) {
        const qint64 fStart = file.offset;
        const qint64 fEnd = file.offset + file.length;

        const qint64 iStart = std::max(start, fStart);
        const qint64 iEnd = std::min(end, fEnd);
        if (iStart >= iEnd) continue;

        WriteRegion region;
        region.file = &file;
        region.fileOffset = iStart - fStart;
        region.length = iEnd - iStart;
        region.bufOffset = iStart - start;
        out.append(region);
    }
    return out;
}

qint64 PieceStore::pieceSize(int piece) const {
    const int pieceCount = m_meta.pieceHashes.size();
    if (piece < 0 || piece >= pieceCount) return 0;
    if (piece == pieceCount - 1)
        return m_meta.totalLength - qint64(piece) * m_meta.pieceLength;
    return m_meta.pieceLength;
}

bool PieceStore::pieceIsWanted(int piece) const {
    const qint64 pieceStart = qint64(piece) * m_meta.pieceLength;
    const qint64 pieceEnd = pieceStart + pieceSize(piece);

    for (int i : m_selectedFiles) {
        if (i < 0 || i >= m_meta.files.size()) continue;
        const FileEntry& file = m_meta.files[i];
        const qint64 fStart = file.offset;
        const qint64 fEnd = file.offset + file.length;
        if (fStart < pieceEnd && fEnd > pieceStart) return true;
    }
    return false;
}

QVector<int> PieceStore::wantedPieces() const {
    QVector<int> out;
    for (int p = 0; p < m_meta.pieceHashes.size(); ++p)
        if (pieceIsWanted(p)) out.append(p);
    return out;
}

QString PieceStore::destRootPath() const {
    return QDir(m_destDir).filePath(m_meta.name);
}

QString PieceStore::absolutePathFor(const FileEntry& file) const {
    if (!m_meta.isMultiFile) return destRootPath();
    return QDir(destRootPath()).filePath(file.path);
}

bool PieceStore::writePiece(int piece, const QByteArray& data, QString* err) {
    const qint64 expected = pieceSize(piece);
    if (data.size() != expected) {
        if (err) *err = QStringLiteral("writePiece: data size %1 does not match expected piece size %2")
                             .arg(data.size()).arg(expected);
        return false;
    }

    const auto regions = regionsFor(piece, 0, expected);
    for (const auto& region : regions) {
        const QString path = absolutePathFor(*region.file);

        QFileInfo fi(path);
        QDir dir = fi.dir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            if (err) *err = QStringLiteral("writePiece: failed to create directory %1").arg(dir.path());
            return false;
        }

        QFile f(path);
        if (!f.open(QIODevice::ReadWrite)) {
            if (err) *err = QStringLiteral("writePiece: failed to open %1: %2").arg(path, f.errorString());
            return false;
        }
        if (!f.seek(region.fileOffset)) {
            if (err) *err = QStringLiteral("writePiece: failed to seek in %1: %2").arg(path, f.errorString());
            return false;
        }

        const QByteArray chunk = data.mid(region.bufOffset, region.length);
        if (f.write(chunk) != chunk.size()) {
            if (err) *err = QStringLiteral("writePiece: short write to %1: %2").arg(path, f.errorString());
            return false;
        }
    }
    return true;
}

bool PieceStore::verify(int piece, const QByteArray& data) const {
    if (piece < 0 || piece >= m_meta.pieceHashes.size()) return false;
    return QCryptographicHash::hash(data, QCryptographicHash::Sha1) == m_meta.pieceHashes[piece];
}

bool PieceStore::verifyOnDisk(int piece, QString* err) const {
    const qint64 size = pieceSize(piece);
    QByteArray buffer(size, char(0));

    const auto regions = regionsFor(piece, 0, size);
    for (const auto& region : regions) {
        const QString path = absolutePathFor(*region.file);

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (err) *err = QStringLiteral("verifyOnDisk: failed to open %1: %2").arg(path, f.errorString());
            return false;
        }
        if (!f.seek(region.fileOffset)) {
            if (err) *err = QStringLiteral("verifyOnDisk: failed to seek in %1: %2").arg(path, f.errorString());
            return false;
        }

        const QByteArray chunk = f.read(region.length);
        if (chunk.size() != region.length) {
            if (err) *err = QStringLiteral("verifyOnDisk: short read from %1").arg(path);
            return false;
        }
        buffer.replace(region.bufOffset, region.length, chunk);
    }

    return verify(piece, buffer);
}

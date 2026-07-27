#include "torrent/TorrentMetainfo.h"
#include "torrent/Bencode.h"

#include <QCryptographicHash>
#include <QFileInfo>

namespace {

bool fail(bool* ok, QString* err, const QString& msg) {
    if (ok) *ok = false;
    if (err) *err = msg;
    return false;
}

} // namespace

TorrentMetainfo TorrentMetainfo::parse(const QByteArray& torrentBytes, bool* ok, QString* err) {
    TorrentMetainfo m;
    if (err) err->clear();

    bool decodeOk = false;
    QString decodeErr;
    BencodeValue root = Bencode::decode(torrentBytes, &decodeOk, &decodeErr);
    if (!decodeOk) {
        fail(ok, err, QString("invalid bencode: %1").arg(decodeErr));
        return {};
    }
    if (root.type() != BencodeValue::Type::Dict) {
        fail(ok, err, "root value is not a dict");
        return {};
    }

    if (!root.contains(QByteArray("info")) || root[QByteArray("info")].type() != BencodeValue::Type::Dict) {
        fail(ok, err, "missing or malformed 'info' dict");
        return {};
    }
    const BencodeValue& info = root[QByteArray("info")];

    // info-hash: SHA-1 over the EXACT raw bytes of the info dict as they
    // appeared in the input, never a re-encoded copy.
    const int infoBegin = info.rawBegin();
    const int infoEnd = info.rawEnd();
    if (infoBegin < 0 || infoEnd < infoBegin || infoEnd > torrentBytes.size()) {
        fail(ok, err, "invalid raw span for 'info' dict");
        return {};
    }
    m.infoHash = QCryptographicHash::hash(torrentBytes.mid(infoBegin, infoEnd - infoBegin),
                                          QCryptographicHash::Sha1);

    if (!info.contains(QByteArray("piece length")) ||
        info[QByteArray("piece length")].type() != BencodeValue::Type::Int) {
        fail(ok, err, "missing or malformed 'piece length'");
        return {};
    }
    m.pieceLength = info[QByteArray("piece length")].toInt();
    if (m.pieceLength <= 0) {
        fail(ok, err, "'piece length' must be positive");
        return {};
    }

    if (!info.contains(QByteArray("pieces")) || info[QByteArray("pieces")].type() != BencodeValue::Type::Bytes) {
        fail(ok, err, "missing or malformed 'pieces'");
        return {};
    }
    const QByteArray pieces = info[QByteArray("pieces")].toBytes();
    if (pieces.size() % 20 != 0) {
        fail(ok, err, "'pieces' length is not a multiple of 20");
        return {};
    }
    m.pieceHashes.clear();
    m.pieceHashes.reserve(pieces.size() / 20);
    for (int i = 0; i < pieces.size(); i += 20)
        m.pieceHashes.append(pieces.mid(i, 20));

    if (!info.contains(QByteArray("name")) || info[QByteArray("name")].type() != BencodeValue::Type::Bytes) {
        fail(ok, err, "missing or malformed 'name'");
        return {};
    }
    m.name = QString::fromUtf8(info[QByteArray("name")].toBytes());

    // "name" is, by spec, a single path component used to build the
    // destination root (single-file: <destDir>/<name>; multi-file:
    // <destDir>/<name>/...). Reject anything that could escape destDir when
    // joined onto it downstream (empty, '.', '..', an embedded separator, or
    // an absolute path), the same structural check already applied to
    // per-file path segments below.
    if (m.name.isEmpty() || m.name == "." || m.name == ".." ||
        m.name.contains('/') || m.name.contains('\\') ||
        QFileInfo(m.name).isAbsolute()) {
        fail(ok, err, "invalid 'name' (empty, '.', '..', contains a separator, or absolute)");
        return {};
    }

    const bool hasLength = info.contains(QByteArray("length"));
    const bool hasFiles = info.contains(QByteArray("files"));

    if (hasLength && hasFiles) {
        // BEP3: exactly one of "length" (single-file) or "files" (multi-file)
        // may be present, never both.
        fail(ok, err, "info has both length and files");
        return {};
    }

    if (hasFiles) {
        // Multi-file mode: "files" is a list of {length, path:[segments]} dicts.
        if (info[QByteArray("files")].type() != BencodeValue::Type::List) {
            fail(ok, err, "'files' is not a list");
            return {};
        }
        m.isMultiFile = true;
        qint64 runningOffset = 0;
        for (const BencodeValue& fileEntry : info[QByteArray("files")].toList()) {
            if (fileEntry.type() != BencodeValue::Type::Dict) {
                fail(ok, err, "'files' entry is not a dict");
                return {};
            }
            if (!fileEntry.contains(QByteArray("length")) ||
                fileEntry[QByteArray("length")].type() != BencodeValue::Type::Int) {
                fail(ok, err, "'files' entry missing or malformed 'length'");
                return {};
            }
            const qint64 fileLength = fileEntry[QByteArray("length")].toInt();
            if (fileLength < 0) {
                fail(ok, err, "'files' entry has negative 'length'");
                return {};
            }
            if (!fileEntry.contains(QByteArray("path")) ||
                fileEntry[QByteArray("path")].type() != BencodeValue::Type::List ||
                fileEntry[QByteArray("path")].toList().isEmpty()) {
                fail(ok, err, "'files' entry missing or malformed 'path'");
                return {};
            }
            QStringList segments;
            for (const BencodeValue& seg : fileEntry[QByteArray("path")].toList()) {
                if (seg.type() != BencodeValue::Type::Bytes) {
                    fail(ok, err, "'path' segment is not a byte string");
                    return {};
                }
                const QByteArray segBytes = seg.toBytes();
                // Reject structurally-invalid segments so a downstream disk
                // writer can't be tricked into escaping the destination
                // directory (empty segment, "." / ".." traversal, or an
                // embedded separator smuggling an absolute/relative path
                // inside what must be a single path component).
                if (segBytes.isEmpty() || segBytes == "." || segBytes == ".." ||
                    segBytes.contains('/') || segBytes.contains('\\')) {
                    fail(ok, err, "'path' segment is invalid (empty, '.', '..', or contains a separator)");
                    return {};
                }
                segments.append(QString::fromUtf8(segBytes));
            }
            FileEntry entry;
            entry.path = segments.join('/');
            entry.length = fileLength;
            entry.offset = runningOffset;
            m.files.append(entry);
            runningOffset += fileLength;
        }
        m.totalLength = runningOffset;
    } else if (hasLength) {
        // Single-file mode: "length" is the whole file's size, "name" is its filename.
        if (info[QByteArray("length")].type() != BencodeValue::Type::Int) {
            fail(ok, err, "malformed 'length'");
            return {};
        }
        const qint64 length = info[QByteArray("length")].toInt();
        if (length < 0) {
            fail(ok, err, "'length' must not be negative");
            return {};
        }
        m.isMultiFile = false;
        m.totalLength = length;
        FileEntry entry;
        entry.path = m.name;
        entry.length = length;
        entry.offset = 0;
        m.files.append(entry);
    } else {
        fail(ok, err, "'info' dict has neither 'length' nor 'files'");
        return {};
    }

    if (m.files.isEmpty()) {
        fail(ok, err, "no files described in 'info' dict");
        return {};
    }

    // sum(files.length) must be consistent with the number of piece hashes.
    const qint64 expectedPieceCount = (m.totalLength + m.pieceLength - 1) / m.pieceLength;
    if (expectedPieceCount != m.pieceHashes.size()) {
        fail(ok, err, "piece count does not match total length / piece length");
        return {};
    }

    if (!root.contains(QByteArray("announce")) || root[QByteArray("announce")].type() != BencodeValue::Type::Bytes) {
        fail(ok, err, "missing or malformed 'announce'");
        return {};
    }
    m.announce = QUrl(QString::fromUtf8(root[QByteArray("announce")].toBytes()));

    // BEP 12 announce-list: a list of tiers, each tier a list of tracker URL
    // byte-strings. Skip malformed entries/empty tiers. When absent or empty,
    // synthesize a single tier holding the plain `announce` URL so downstream
    // code always sees a uniform tier structure.
    if (root.contains(QByteArray("announce-list")) &&
        root[QByteArray("announce-list")].type() == BencodeValue::Type::List) {
        for (const BencodeValue& tierVal : root[QByteArray("announce-list")].toList()) {
            if (tierVal.type() != BencodeValue::Type::List) continue;
            QVector<QUrl> tier;
            for (const BencodeValue& urlVal : tierVal.toList()) {
                if (urlVal.type() != BencodeValue::Type::Bytes) continue;
                const QUrl u(QString::fromUtf8(urlVal.toBytes()));
                if (u.isValid() && !u.scheme().isEmpty()) tier.append(u);
            }
            if (!tier.isEmpty()) m.announceList.append(tier);
        }
    }
    if (m.announceList.isEmpty() && m.announce.isValid() && !m.announce.scheme().isEmpty())
        m.announceList.append(QVector<QUrl>{m.announce});

    if (ok) *ok = true;
    return m;
}

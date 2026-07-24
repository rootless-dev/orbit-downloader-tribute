#pragma once

#include <QSet>
#include <QString>
#include <QVector>

struct TorrentMetainfo;

// Pure file-selection logic for the "open .torrent" flow: turns a
// TorrentMetainfo's flat file list into a folder/file tree, and maps a set
// of checked tree-node indices back to the file indices they represent.
// No QtWidgets dependency (lives in orbitgui_logic) so it is unit-testable
// headless; TorrentOpenDialog (orbitgui) is the only thing that touches a
// QTreeWidget.
namespace TorrentSelection {

struct Node {
    QString      name;
    qint64       size      = 0;
    int          fileIndex = -1;   // -1 => folder; >=0 => index into TorrentMetainfo::files
    QVector<int> children;         // indices into the QVector<Node> this node lives in
};

// Splits each FileEntry::path on '/' and folds the results into a tree.
// Index 0 is always the root node. For a single-file torrent (isMultiFile
// == false) the result is a single leaf node (fileIndex 0). For a
// multi-file torrent, index 0 is a synthetic root representing the torrent
// as a whole (fileIndex -1) and every path segment is real: paths coming
// out of TorrentMetainfo::parse are plain relative paths (e.g. "a.txt",
// "sub/b.txt") with no shared top-level prefix to fold away, so nothing is
// discarded — a "sub/b.txt" entry produces a real "sub" folder node.
// Folder node sizes are the sum of their descendants' sizes.
QVector<Node> buildTree(const TorrentMetainfo& m);

// Given the full tree and the set of CHECKED node indices, returns the
// file indices (>=0) carried by every checked leaf. Parent/child check
// cascade semantics (checking a folder implies its files, tristate, etc.)
// are the dialog's job; this function only maps checked leaves -> file
// indices.
QSet<int> selectedFileIndices(const QVector<Node>& tree, const QSet<int>& checkedNodeIndices);

} // namespace TorrentSelection

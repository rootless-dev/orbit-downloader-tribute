#include "TorrentSelection.h"
#include "torrent/TorrentMetainfo.h"

#include <QHash>
#include <QStringList>

namespace TorrentSelection {

QVector<Node> buildTree(const TorrentMetainfo& m) {
    QVector<Node> nodes;

    if (!m.isMultiFile) {
        // Single-file torrent: exactly one leaf, no folder wrapper.
        if (m.files.isEmpty()) return nodes;
        Node leaf;
        leaf.name      = m.name;
        leaf.fileIndex = 0;
        leaf.size      = m.files[0].length;
        nodes.push_back(leaf);
        return nodes;
    }

    // Root node (index 0): a synthetic node representing the torrent as a
    // whole. TorrentMetainfo::parse does NOT prepend the torrent name to
    // FileEntry::path (verified against tst_metainfo.cpp's
    // parsesMultiFileOffsets: paths come back as plain relative paths like
    // "a.txt" / "sub/b.txt", no shared top segment) — so every path segment
    // is real and must be walked from position 0. An earlier version of
    // this function assumed a shared top segment and skipped parts[0],
    // which silently collapsed every torrent's outermost subdirectory.
    Node root;
    root.name      = m.name;
    root.fileIndex = -1;
    nodes.push_back(root);

    // Maps a folder's full path prefix (segments joined by '/') -> its node
    // index, so files that share an intermediate directory reuse the same
    // folder node.
    QHash<QString, int> folderByPrefix;

    for (int i = 0; i < m.files.size(); ++i) {
        const FileEntry& f = m.files[i];
        const QStringList parts = f.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        // TorrentMetainfo::parse already rejects paths with empty segments
        // (e.g. "", "a//b"), so this only guards a defensively-empty/blank
        // path a hand-built fixture might pass in directly.
        if (parts.isEmpty()) continue;

        int parentIdx = 0;
        QString prefix;
        for (int p = 0; p < parts.size(); ++p) {
            prefix = prefix.isEmpty() ? parts[p] : prefix + QLatin1Char('/') + parts[p];
            const bool isLeaf = (p == parts.size() - 1);
            if (isLeaf) {
                Node leaf;
                leaf.name      = parts[p];
                leaf.fileIndex = i;
                leaf.size      = f.length;
                const int idx = nodes.size();
                nodes.push_back(leaf);
                nodes[parentIdx].children.push_back(idx);
            } else {
                auto it = folderByPrefix.constFind(prefix);
                if (it == folderByPrefix.constEnd()) {
                    Node folder;
                    folder.name      = parts[p];
                    folder.fileIndex = -1;
                    const int idx = nodes.size();
                    nodes.push_back(folder);
                    nodes[parentIdx].children.push_back(idx);
                    folderByPrefix.insert(prefix, idx);
                    parentIdx = idx;
                } else {
                    parentIdx = it.value();
                }
            }
        }
    }

    // Fold folder sizes bottom-up. Every node's children are appended to
    // the vector strictly after the node itself, so a single descending
    // pass over the index range guarantees a child's final size is already
    // computed before its parent needs it.
    for (int i = nodes.size() - 1; i >= 0; --i) {
        if (nodes[i].fileIndex != -1) continue;   // leaf: size already set
        qint64 total = 0;
        for (int c : nodes[i].children) total += nodes[c].size;
        nodes[i].size = total;
    }

    return nodes;
}

QSet<int> selectedFileIndices(const QVector<Node>& tree, const QSet<int>& checkedNodeIndices) {
    QSet<int> result;
    for (int idx : checkedNodeIndices) {
        if (idx < 0 || idx >= tree.size()) continue;
        if (tree[idx].fileIndex >= 0) result.insert(tree[idx].fileIndex);
    }
    return result;
}

} // namespace TorrentSelection

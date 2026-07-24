#pragma once

#include "DownloadTypes.h"     // PieceStrategy
#include "TorrentSelection.h"
#include "torrent/TorrentMetainfo.h"

#include <QDialog>
#include <QSet>

class QLineEdit;
class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;

// "Open .torrent" dialog: shows the torrent's name/size, a checkable file
// tree (all checked by default, folder checks cascade to children), a
// destination folder picker and a piece-selection-strategy combo. Widget
// logic stays thin here; the tree building and checked->file-index mapping
// are delegated to the pure TorrentSelection functions so that logic is
// unit-tested headless.
class TorrentOpenDialog : public QDialog {
    Q_OBJECT
public:
    explicit TorrentOpenDialog(const TorrentMetainfo& meta,
                                const QString& defaultDir,
                                PieceStrategy defaultStrategy,
                                QWidget* parent = nullptr);

    QString       destDir() const;
    QSet<int>     selectedFiles() const;
    PieceStrategy strategy() const;

private slots:
    void chooseDir();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void populateTree();
    QTreeWidgetItem* buildItem(int nodeIndex);
    void cascadeToChildren(QTreeWidgetItem* item, Qt::CheckState state);
    void updateAncestorState(QTreeWidgetItem* item);

    TorrentMetainfo         m_meta;
    QVector<TorrentSelection::Node> m_nodes;

    QTreeWidget* m_tree;
    QLineEdit*   m_dir;
    QComboBox*   m_strategy;

    bool m_updatingChecks = false;   // guards against re-entrant itemChanged during cascades
};

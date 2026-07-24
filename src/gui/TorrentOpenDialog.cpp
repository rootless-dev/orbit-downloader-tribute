#include "TorrentOpenDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {
constexpr int kNodeIndexRole = Qt::UserRole;
}

TorrentOpenDialog::TorrentOpenDialog(const TorrentMetainfo& meta,
                                     const QString& defaultDir,
                                     PieceStrategy defaultStrategy,
                                     QWidget* parent)
    : QDialog(parent), m_meta(meta) {
    setWindowTitle(tr("Open Torrent"));
    m_nodes = TorrentSelection::buildTree(m_meta);

    auto* nameLabel = new QLabel(m_meta.name, this);
    auto* sizeLabel = new QLabel(QLocale().formattedDataSize(m_meta.totalLength), this);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName("fileTree");
    m_tree->setHeaderLabels({tr("Name"), tr("Size")});
    m_tree->setColumnCount(2);
    populateTree();
    connect(m_tree, &QTreeWidget::itemChanged, this, &TorrentOpenDialog::onItemChanged);

    m_dir = new QLineEdit(defaultDir, this);
    m_dir->setObjectName("destDirEdit");
    auto* browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &TorrentOpenDialog::chooseDir);
    auto* dirRow = new QWidget(this);
    auto* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    dirLay->addWidget(m_dir);
    dirLay->addWidget(browse);

    m_strategy = new QComboBox(this);
    m_strategy->setObjectName("strategyCombo");
    m_strategy->addItem(tr("Rarest First"), int(PieceStrategy::RarestFirst));
    m_strategy->addItem(tr("Sequential"), int(PieceStrategy::Sequential));
    m_strategy->setCurrentIndex(defaultStrategy == PieceStrategy::Sequential ? 1 : 0);

    auto* form = new QFormLayout;
    form->addRow(tr("Name:"), nameLabel);
    form->addRow(tr("Total size:"), sizeLabel);
    form->addRow(tr("Save to:"), dirRow);
    form->addRow(tr("Strategy:"), m_strategy);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_tree);
    layout->addWidget(box);
    resize(480, 420);
}

QTreeWidgetItem* TorrentOpenDialog::buildItem(int nodeIndex) {
    const TorrentSelection::Node& node = m_nodes[nodeIndex];
    auto* item = new QTreeWidgetItem();
    item->setText(0, node.name);
    item->setText(1, QLocale().formattedDataSize(node.size));
    item->setData(0, kNodeIndexRole, nodeIndex);
    // Manual cascade (onItemChanged) drives parent/child check state, so
    // Qt::ItemIsAutoTristate is deliberately NOT set here: Qt's own
    // auto-computed tristate would otherwise fight with our explicit
    // setCheckState() calls on folder items.
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, Qt::Checked);   // all checked by default

    for (int childIdx : node.children) item->addChild(buildItem(childIdx));
    return item;
}

void TorrentOpenDialog::populateTree() {
    m_tree->clear();
    if (m_nodes.isEmpty()) return;
    m_tree->blockSignals(true);
    m_tree->addTopLevelItem(buildItem(0));
    m_tree->expandAll();
    m_tree->blockSignals(false);
}

void TorrentOpenDialog::cascadeToChildren(QTreeWidgetItem* item, Qt::CheckState state) {
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        child->setCheckState(0, state);   // triggers itemChanged -> recurses further down
    }
}

void TorrentOpenDialog::updateAncestorState(QTreeWidgetItem* item) {
    QTreeWidgetItem* parent = item->parent();
    while (parent) {
        int checkedCount = 0, uncheckedCount = 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            switch (parent->child(i)->checkState(0)) {
                case Qt::Checked:   ++checkedCount; break;
                case Qt::Unchecked: ++uncheckedCount; break;
                default: break; // PartiallyChecked child -> parent is partial too
            }
        }
        Qt::CheckState next;
        if (checkedCount == parent->childCount()) next = Qt::Checked;
        else if (uncheckedCount == parent->childCount()) next = Qt::Unchecked;
        else next = Qt::PartiallyChecked;
        parent->setCheckState(0, next);
        parent = parent->parent();
    }
}

void TorrentOpenDialog::onItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0 || m_updatingChecks) return;
    m_updatingChecks = true;
    const Qt::CheckState state = item->checkState(0);
    if (state != Qt::PartiallyChecked) cascadeToChildren(item, state);
    updateAncestorState(item);
    m_updatingChecks = false;
}

QString TorrentOpenDialog::destDir() const { return m_dir->text(); }

PieceStrategy TorrentOpenDialog::strategy() const {
    return m_strategy->currentIndex() == 1 ? PieceStrategy::Sequential : PieceStrategy::RarestFirst;
}

QSet<int> TorrentOpenDialog::selectedFiles() const {
    QSet<int> checkedNodes;
    for (auto it = QTreeWidgetItemIterator(m_tree); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->checkState(0) == Qt::Checked || item->checkState(0) == Qt::PartiallyChecked)
            checkedNodes.insert(item->data(0, kNodeIndexRole).toInt());
    }
    return TorrentSelection::selectedFileIndices(m_nodes, checkedNodes);
}

void TorrentOpenDialog::chooseDir() {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Save to"), m_dir->text());
    if (!d.isEmpty()) m_dir->setText(d);
}

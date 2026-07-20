#include "CategoryTree.h"

using F = CategoryFilterProxy::Filter;

static QTreeWidgetItem* node(const QString& text, F f) {
    auto* it = new QTreeWidgetItem(QStringList{text});
    it->setData(0, Qt::UserRole, QVariant::fromValue(f));
    return it;
}

CategoryTree::CategoryTree(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    auto* root = node("All Downloads", F::All);
    root->addChild(node("Downloading", F::Downloading)); // index 0
    root->addChild(node("Completed",   F::Completed));   // index 1
    root->addChild(node("Movie",       F::Movie));       // index 2
    root->addChild(node("Software",    F::Software));     // index 3
    root->addChild(node("Music",       F::Music));        // index 4
    root->addChild(node("Others",      F::Others));       // index 5
    addTopLevelItem(root);
    expandAll();

    connect(this, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
            if (!cur) return;
            emit filterChanged(cur->data(0, Qt::UserRole).value<F>());
        });

    setCurrentItem(root);
}

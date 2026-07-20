#pragma once
#include "CategoryFilterProxy.h"
#include <QTreeWidget>

class CategoryTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit CategoryTree(QWidget* parent = nullptr);
signals:
    void filterChanged(CategoryFilterProxy::Filter f);
};

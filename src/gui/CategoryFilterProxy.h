#pragma once
#include <QMetaType>
#include <QSortFilterProxyModel>

class CategoryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class Filter { All, Downloading, Completed, Movie, Software, Music, Others };
    void setFilter(Filter f);
protected:
    bool filterAcceptsRow(int srcRow, const QModelIndex& srcParent) const override;
private:
    Filter m_filter = Filter::All;
};
Q_DECLARE_METATYPE(CategoryFilterProxy::Filter)   // needed for QVariant::fromValue + QSignalSpy (Task 11)

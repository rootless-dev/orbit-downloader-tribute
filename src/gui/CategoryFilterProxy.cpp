#include "CategoryFilterProxy.h"
#include "DownloadTableModel.h"
#include "FileType.h"
#include "DownloadTypes.h"

void CategoryFilterProxy::setFilter(Filter f) {
    beginFilterChange();
    m_filter = f;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool CategoryFilterProxy::filterAcceptsRow(int srcRow, const QModelIndex& parent) const {
    if (m_filter == Filter::All) return true;
    const QModelIndex ix = sourceModel()->index(srcRow, 0, parent);
    const auto state = DownloadState(ix.data(DownloadTableModel::StateRole).toInt());
    const auto cat   = FileType::Category(ix.data(DownloadTableModel::CategoryRole).toInt());
    switch (m_filter) {
        case Filter::Downloading: return state != DownloadState::Completed;
        case Filter::Completed:   return state == DownloadState::Completed;
        case Filter::Movie:       return cat == FileType::Category::Movie;
        case Filter::Software:    return cat == FileType::Category::Software;
        case Filter::Music:       return cat == FileType::Category::Music;
        case Filter::Others:      return cat == FileType::Category::Others;
        case Filter::All:         return true;
    }
    return true;
}

#include "DownloadTableModel.h"
#include "DownloadManager.h"
#include "DownloadTask.h"
#include "FileType.h"
#include <QFileInfo>
#include <QLocale>

DownloadTableModel::DownloadTableModel(DownloadManager* mgr, QObject* parent)
    : QAbstractTableModel(parent), m_mgr(mgr) {
    for (DownloadTask* t : m_mgr->tasks()) appendTask(t);
    connect(m_mgr, &DownloadManager::taskProgress,     this, &DownloadTableModel::onTaskProgress);
    connect(m_mgr, &DownloadManager::taskStateChanged, this, &DownloadTableModel::onTaskStateChanged);
    m_clock.start();
    connect(&m_tick, &QTimer::timeout, this, &DownloadTableModel::onSpeedTick);
    m_tick.start(1000);
}

int DownloadTableModel::rowCount(const QModelIndex&) const  { return m_rows.size(); }
int DownloadTableModel::columnCount(const QModelIndex&) const { return ColumnCount; }
DownloadTask* DownloadTableModel::taskAt(int row) const {
    if (row < 0 || row >= m_rows.size()) return nullptr;
    return m_rows[row].task;
}
int DownloadTableModel::rowForId(const QUuid& id) const { return m_index.value(id, -1); }

void DownloadTableModel::appendTask(DownloadTask* t) {
    const int row = m_rows.size();
    beginInsertRows({}, row, row);
    Row r; r.task = t; r.total = t->record().totalBytes;
    qint64 rx = 0;
    for (const Segment& s : t->segments()) rx += s.downloaded();
    r.received = rx;
    m_rows.push_back(r);
    m_index.insert(t->id(), row);
    endInsertRows();
}

void DownloadTableModel::removeTaskById(const QUuid& id) {
    const int row = rowForId(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    m_rows.remove(row);
    m_index.remove(id);
    for (int i = row; i < m_rows.size(); ++i) m_index[m_rows[i].task->id()] = i;
    endRemoveRows();
}

void DownloadTableModel::onTaskProgress(const QUuid& id, qint64 received, qint64 total) {
    const int row = rowForId(id);
    if (row < 0) return;
    m_rows[row].received = received;
    if (total > 0) m_rows[row].total = total;
    emit dataChanged(index(row, Size), index(row, Progress), {Qt::DisplayRole, ProgressRole});
}

void DownloadTableModel::onTaskStateChanged(const QUuid& id, DownloadState s) {
    const int row = rowForId(id);
    if (row < 0) return;
    if (s == DownloadState::Completed || s == DownloadState::Error) m_rows[row].sampler.reset();
    emit dataChanged(index(row, Status), index(row, TimeLeft), {Qt::DisplayRole, StateRole});
    // Refresh separado da coluna Name: o destino pode ter sido renomeado no
    // probe (Content-Disposition p/ downloads provisórios) entre a criação da
    // linha e a transição p/ Downloading — sem isto a tabela mostraria o nome
    // antigo. Emit próprio p/ não alargar o range acima (que os testes fixam).
    emit dataChanged(index(row, Name), index(row, Name), {Qt::DisplayRole});
}

void DownloadTableModel::onSpeedTick() {
    const qint64 now = m_clock.elapsed();
    for (int row = 0; row < m_rows.size(); ++row) {
        Row& r = m_rows[row];
        if (r.task->state() != DownloadState::Downloading) continue;
        r.sampler.addSample(r.received, now);
        emit dataChanged(index(row, Speed), index(row, TimeLeft), {Qt::DisplayRole});
    }
}

static QString stateText(DownloadState s) {
    switch (s) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
    }
    return {};
}

QVariant DownloadTableModel::data(const QModelIndex& ix, int role) const {
    if (!ix.isValid() || ix.row() >= m_rows.size()) return {};
    const Row& r = m_rows[ix.row()];
    const auto rec = r.task->record();
    const QString name = QFileInfo(rec.destPath).fileName();

    if (role == TaskRole)     return QVariant::fromValue(r.task);
    if (role == StateRole)    return int(r.task->state());
    if (role == CategoryRole) return int(FileType::categorize(name));
    if (role == ProgressRole)
        return r.total > 0 ? int(100 * r.received / r.total) : 0;

    if (role != Qt::DisplayRole) return {};
    switch (ix.column()) {
        case Name:   return name;
        case Size:   return r.total > 0 ? QLocale().formattedDataSize(r.total) : QString("—");
        case Progress: return r.total > 0 ? QString::number(100 * r.received / r.total) + "%"
                                          : QString("—");
        case Status: return stateText(r.task->state());
        case Speed: {
            if (r.task->state() != DownloadState::Downloading) return QString();
            const double bps = r.sampler.bytesPerSec();
            return bps > 0 ? QLocale().formattedDataSize(qint64(bps)) + "/s" : QString();
        }
        case TimeLeft: {
            if (r.task->state() != DownloadState::Downloading) return QString();
            const qint64 eta = r.sampler.etaSeconds(r.total);
            if (eta < 0) return QString("—");
            return QString("%1:%2").arg(eta/60, 2, 10, QChar('0')).arg(eta%60, 2, 10, QChar('0'));
        }
        case Priority: return priorityToString(r.task->priority());
    }
    return {};
}

QVariant DownloadTableModel::headerData(int s, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* h[] = {"Name","Size","Progress","Status","Speed","Time Left","Priority"};
    return (s >= 0 && s < ColumnCount) ? QString(h[s]) : QVariant();
}

void DownloadTableModel::refreshRow(const QUuid& id) {
    const int row = rowForId(id);
    if (row < 0) return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

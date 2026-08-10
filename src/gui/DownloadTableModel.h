#pragma once
#include "DownloadTypes.h"
#include "SpeedSampler.h"
#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QVector>
class DownloadManager;
class AbstractTask;

class DownloadTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Progress, Status, Speed, TimeLeft, Priority, ColumnCount };
    enum Roles  { StateRole = Qt::UserRole + 1, CategoryRole, ProgressRole, TaskRole };
    explicit DownloadTableModel(DownloadManager* mgr, QObject* parent = nullptr);
    int      rowCount(const QModelIndex& = {}) const override;
    int      columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int) const override;
    void          appendTask(AbstractTask* t);
    void          removeTaskById(const QUuid& id);
    void          refreshRow(const QUuid& id);
    AbstractTask* taskAt(int row) const;
    // Final-review Fix C1: re-points an existing row's task pointer (e.g. a
    // magnet's transient MagnetTask placeholder replaced, same id, by the
    // real TorrentTask onMetadataReady() built - see DownloadManager's
    // taskReplaced signal) WITHOUT touching row identity/index (unlike
    // removeTaskById()+appendTask(), which would reorder the table and lose
    // the user's current selection). Resets the speed sampler (the new task
    // starts its own progress/state history from scratch) and re-reads
    // total/received off the new task so Size/Progress stop reflecting the
    // placeholder's "unknown" (-1/0) values. No-op if `id` isn't a known row.
    void          retargetTask(const QUuid& id, AbstractTask* task);
private slots:
    void onTaskProgress(const QUuid& id, qint64 received, qint64 total);
    void onTaskStateChanged(const QUuid& id, DownloadState s);
    void onSpeedTick();
private:
    // QPointer, not a plain AbstractTask*: defense-in-depth against a missed/
    // future-mis-wired taskReplaced signal (Fix C1) - a task deleted out from
    // under a row degrades this to a null-guarded stale row (data() below
    // returns {} instead of dereferencing) rather than a use-after-free.
    struct Row { QPointer<AbstractTask> task; qint64 received = 0; qint64 total = -1; SpeedSampler sampler; };
    int rowForId(const QUuid& id) const;
    DownloadManager*  m_mgr;
    QVector<Row>      m_rows;
    QHash<QUuid,int>  m_index;      // id -> row
    QTimer            m_tick;
    QElapsedTimer     m_clock;
};

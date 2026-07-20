#pragma once
#include "DownloadTypes.h"
#include "SpeedSampler.h"
#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <QVector>
class DownloadManager;
class DownloadTask;

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
    void          appendTask(DownloadTask* t);
    void          removeTaskById(const QUuid& id);
    void          refreshRow(const QUuid& id);
    DownloadTask* taskAt(int row) const;
private slots:
    void onTaskProgress(const QUuid& id, qint64 received, qint64 total);
    void onTaskStateChanged(const QUuid& id, DownloadState s);
    void onSpeedTick();
private:
    struct Row { DownloadTask* task; qint64 received = 0; qint64 total = -1; SpeedSampler sampler; };
    int rowForId(const QUuid& id) const;
    DownloadManager*  m_mgr;
    QVector<Row>      m_rows;
    QHash<QUuid,int>  m_index;      // id -> row
    QTimer            m_tick;
    QElapsedTimer     m_clock;
};

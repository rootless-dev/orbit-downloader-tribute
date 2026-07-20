#pragma once
#include "DownloadTypes.h"
#include <QVector>

enum class CellKind { Pending, Downloaded, Error, Active };
struct Cell {
    CellKind kind         = CellKind::Pending;
    int      segmentIndex = -1;
};
QVector<Cell> computeCells(qint64 totalBytes,
                           const QVector<Segment>& segments,
                           DownloadState state,
                           int nCells);

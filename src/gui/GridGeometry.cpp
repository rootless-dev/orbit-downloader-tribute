#include "GridGeometry.h"

static int ownerSegment(qint64 byte, const QVector<Segment>& segs) {
    for (const Segment& s : segs)
        if (byte >= s.start && (s.end < 0 || byte <= s.end)) return s.index;
    return -1;
}

QVector<Cell> computeCells(qint64 totalBytes,
                           const QVector<Segment>& segments,
                           DownloadState state,
                           int nCells) {
    QVector<Cell> cells;
    if (nCells <= 0) return cells;
    cells.resize(nCells);
    if (totalBytes <= 0) return cells;   // all Pending (indeterminate)

    for (int i = 0; i < nCells; ++i) {
        const qint64 cellStart = static_cast<qint64>(i)     * totalBytes / nCells;
        const qint64 cellEnd   = static_cast<qint64>(i + 1) * totalBytes / nCells; // exclusive
        const int owner = ownerSegment(cellStart, segments);

        // Downloaded iff every segment overlapping [cellStart, cellEnd) has
        // fetched its whole portion of the cell. This covers cells that
        // straddle a segment boundary (the old code only checked the segment
        // owning cellStart, leaving boundary cells gray at 100%).
        bool anyCover = false;
        bool downloaded = true;
        for (const Segment& s : segments) {
            const qint64 sEndExcl = (s.end < 0) ? cellEnd : s.end + 1; // exclusive end
            const bool overlaps = s.start < cellEnd && sEndExcl > cellStart;
            if (!overlaps) continue;
            anyCover = true;
            if (s.current < qMin(cellEnd, sEndExcl)) { downloaded = false; break; }
        }
        downloaded = downloaded && anyCover;
        if (state == DownloadState::Completed) downloaded = true;  // defensive fill

        qint64 cur = -1;
        for (const Segment& s : segments)
            if (s.index == owner) { cur = s.current; break; }
        const bool active = (state == DownloadState::Downloading ||
                             state == DownloadState::Connecting);
        if (downloaded) {
            cells[i].kind = CellKind::Downloaded;
            cells[i].segmentIndex = owner;
        } else if (active && owner >= 0 && cellStart <= cur && cur < cellEnd) {
            cells[i].kind = CellKind::Active;         // célula que contém o current
            cells[i].segmentIndex = owner;
        } else if (state == DownloadState::Error) {
            cells[i].kind = CellKind::Error;
        } else {
            cells[i].kind = CellKind::Pending;
        }
    }
    return cells;
}

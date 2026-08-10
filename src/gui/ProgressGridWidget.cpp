#include "ProgressGridWidget.h"
#include "GridGeometry.h"
#include "AbstractTask.h"
#include "DownloadTask.h"
#include "torrent/TorrentTask.h"
#include "Theme.h"
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>

ProgressGridWidget::ProgressGridWidget(QWidget* parent) : QWidget(parent) {
    m_repaint.setSingleShot(true);
    connect(&m_repaint, &QTimer::timeout, this, [this]{ update(); });
}

void ProgressGridWidget::setTask(AbstractTask* t) {
    if (m_task) disconnect(m_task, nullptr, this, nullptr);
    m_task = t;
    m_dl   = qobject_cast<DownloadTask*>(t);
    m_tor  = qobject_cast<TorrentTask*>(t);
    if (m_dl) {
        // progresso: só repinta (o total de tiles não muda com os bytes).
        connect(m_dl, &DownloadTask::segmentProgress, this, [this]{ scheduleRepaint(); });
    } else if (m_tor) {
        // cada peça verificada/em-voo muda uma célula -> repinta (o total de
        // células é fixo pelo nº de peças, então basta repintar).
        connect(m_tor, &TorrentTask::pieceStateChanged, this, [this]{ scheduleRepaint(); });
    }
    if (m_task) {
        // troca de estado pode revelar o totalBytes (após a sondagem) -> o total
        // de tiles muda, então recomputa a altura, não só repinta.
        connect(m_task, &AbstractTask::stateChanged, this, [this]{ relayout(); update(); });
    }
    relayout();
    update();
}

void ProgressGridWidget::scheduleRepaint() {
    if (!m_repaint.isActive()) m_repaint.start(100);   // throttle repaints
}

int ProgressGridWidget::viewportHeight() const {
    // Dentro de um QScrollArea (setWidgetResizable), o pai imediato é o viewport;
    // a altura dele é a área REALMENTE visível. A altura do próprio widget pode
    // ser maior — é o que gera a barra de rolagem.
    return parentWidget() ? parentWidget()->height() : height();
}

// Total de tiles. Torrent: uma célula por peça (agregando ceil(pieceCount/
// tileBudget) peças por célula em torrents enormes). Byte: um tile por bloco
// de ~kBlockBytes, limitado, e enquanto o tamanho é desconhecido preenche a
// área visível. Não depende da largura da janela — redimensionar só reflui.
int ProgressGridWidget::cellCount() const {
    if (m_tor) {
        const int pc = m_tor->metainfo().pieceHashes.size();
        if (pc <= 0) return 0;
        const int piecesPerCell = (pc + kMaxTiles - 1) / kMaxTiles;   // ceil(pc/tileBudget)
        return (pc + piecesPerCell - 1) / piecesPerCell;              // ceil(pc/piecesPerCell)
    }
    if (!m_dl) return 0;
    const qint64 total = m_dl->record().totalBytes;
    if (total <= 0) {
        const int cols = qMax(1, width() / kCellPx);
        const int rows = qMax(1, viewportHeight() / kCellPx);
        return cols * rows;
    }
    const qint64 n = total / kBlockBytes;
    if (n < kMinTiles) return kMinTiles;
    if (n > kMaxTiles) return kMaxTiles;
    return int(n);
}

// Tiles a pintar. O byte grid preenche o resto da linha (qMax com as colunas)
// para não deixar uma última linha "picada"; o torrent pinta exatamente uma
// célula por grupo de peças (sem padding) — assim a grade é fiel ao nº de peças.
int ProgressGridWidget::layoutCellCount() const {
    if (m_tor) return cellCount();
    const int cols = qMax(1, width() / kCellPx);
    return qMax(cellCount(), cols);
}

void ProgressGridWidget::relayout() {
    const int cols    = qMax(1, width() / kCellPx);
    const int fitRows = qMax(1, viewportHeight() / kCellPx);
    const int n       = cellCount();
    int rows = (n + cols - 1) / cols;             // ceil, na largura ATUAL
    // Absorve o feedback de ~1 barra de largura no limiar de caber (a barra
    // "come" ~kCellPx e sozinha empurraria 1 linha extra, mantendo-a num loop).
    const int sbCols    = style()->pixelMetric(QStyle::PM_ScrollBarExtent) / kCellPx + 1;
    const int colsToFit = (n + fitRows - 1) / fitRows;
    if (cols + sbCols >= colsToFit) rows = qMin(rows, fitRows);
    setMinimumHeight(rows * kCellPx);             // > viewport => QScrollArea rola
}

void ProgressGridWidget::resizeEvent(QResizeEvent*) {
    relayout();
}

// Peça -> célula (Task 14). Mapeia [0, pieceCount) sobre nCells por faixas
// proporcionais (como computeCells faz com bytes). Cor da célula: Have em todas
// -> "have/done" (Downloaded); qualquer InFlight -> Active; senão Pending.
QVector<Cell> ProgressGridWidget::computeTorrentCells(int nCells) const {
    QVector<Cell> cells;
    if (nCells <= 0 || !m_tor) return cells;
    cells.resize(nCells);
    const int pc = m_tor->metainfo().pieceHashes.size();
    if (pc <= 0) return cells;
    for (int i = 0; i < nCells; ++i) {
        const int a = int(static_cast<qint64>(i)     * pc / nCells);
        const int b = int(static_cast<qint64>(i + 1) * pc / nCells);
        bool anyPiece = false, allHave = true, anyInFlight = false;
        for (int p = a; p < b; ++p) {
            anyPiece = true;
            const PieceState st = m_tor->pieceState(p);
            if (st != PieceState::Have)     allHave = false;
            if (st == PieceState::InFlight) anyInFlight = true;
        }
        if (anyPiece && allHave)  cells[i].kind = CellKind::Downloaded;
        else if (anyInFlight)     cells[i].kind = CellKind::Active;
        else                      cells[i].kind = CellKind::Pending;
    }
    return cells;
}

QVector<Cell> ProgressGridWidget::computeCurrentCells() const {
    const int nCells = layoutCellCount();
    if (m_tor) return computeTorrentCells(nCells);
    if (m_dl) {
        const auto rec = m_dl->record();
        return computeCells(rec.totalBytes, m_dl->segments(), m_dl->state(), nCells);
    }
    return {};
}

void ProgressGridWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const GridColors col = gridColors();
    p.fillRect(rect(), col.background);
    if (!m_dl && !m_tor) return;
    const int cols  = qMax(1, width() / kCellPx);
    const auto cells = computeCurrentCells();
    for (int i = 0; i < cells.size(); ++i) {
        const int cx = (i % cols) * kCellPx;
        const int cy = (i / cols) * kCellPx;
        QColor c;
        switch (cells[i].kind) {
            case CellKind::Downloaded: c = col.downloaded; break;
            case CellKind::Active:     c = col.active;     break;
            case CellKind::Error:      c = col.error;      break;
            case CellKind::Pending:    c = col.pending;    break;
        }
        p.fillRect(cx, cy, kCellPx - 1, kCellPx - 1, c);
    }
}

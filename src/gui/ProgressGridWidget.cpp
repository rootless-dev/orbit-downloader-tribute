#include "ProgressGridWidget.h"
#include "GridGeometry.h"
#include "DownloadTask.h"
#include "Theme.h"
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>

ProgressGridWidget::ProgressGridWidget(QWidget* parent) : QWidget(parent) {
    m_repaint.setSingleShot(true);
    connect(&m_repaint, &QTimer::timeout, this, [this]{ update(); });
}

void ProgressGridWidget::setTask(DownloadTask* t) {
    if (m_task) disconnect(m_task, nullptr, this, nullptr);
    m_task = t;
    if (m_task) {
        // progresso: só repinta (o total de tiles não muda com os bytes).
        connect(m_task, &DownloadTask::segmentProgress, this, [this]{ scheduleRepaint(); });
        // troca de estado pode revelar o totalBytes (após a sondagem) -> o total
        // de tiles muda, então recomputa a altura, não só repinta.
        connect(m_task, &DownloadTask::stateChanged, this, [this]{ relayout(); update(); });
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

// Total de tiles: FIXO pelo tamanho do arquivo (um tile por bloco de ~kBlockBytes,
// limitado). Não depende do tamanho da janela — redimensionar só reflui/rola.
// Enquanto o tamanho é desconhecido (sondando), preenche a área visível.
int ProgressGridWidget::cellCount() const {
    if (!m_task) return 0;
    const qint64 total = m_task->record().totalBytes;
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

void ProgressGridWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const GridColors col = gridColors();
    p.fillRect(rect(), col.background);
    if (!m_task) return;
    const int cols   = qMax(1, width() / kCellPx);
    const int nCells = qMax(cellCount(), cols);
    const auto rec   = m_task->record();
    const auto cells = computeCells(rec.totalBytes, m_task->segments(), m_task->state(), nCells);
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

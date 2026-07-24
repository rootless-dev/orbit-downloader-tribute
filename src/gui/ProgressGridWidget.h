#pragma once
#include <QWidget>
#include <QTimer>
#include "GridGeometry.h"   // Cell / CellKind
class AbstractTask;
class DownloadTask;
class TorrentTask;

class ProgressGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProgressGridWidget(QWidget* parent = nullptr);
    // Accepts any task kind. Byte-segment downloads (DownloadTask) render
    // exactly as before; a TorrentTask renders one cell per piece (aggregated
    // for very large torrents). Non-DownloadTask/non-TorrentTask (or nullptr)
    // clears the grid.
    void setTask(AbstractTask* t);
    QVector<Cell> cellsForTest() const { return computeCurrentCells(); }   // test hook
    QSize sizeHint() const override { return QSize(240, 160); }        // painel não colapsa
    QSize minimumSizeHint() const override { return QSize(40, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void scheduleRepaint();
    void relayout();          // ajusta a altura do widget p/ o nº de tiles/colunas atuais
    int  cellCount() const;   // total de tiles: bytes (arquivo) OU peças (torrent)
    int  layoutCellCount() const;               // tiles a pintar (byte grid preenche a linha)
    QVector<Cell> computeCurrentCells() const;  // células p/ o tamanho/estado atuais
    QVector<Cell> computeTorrentCells(int nCells) const;
    int  viewportHeight() const;
    AbstractTask* m_task = nullptr;   // base ponteiro (p/ (dis)connect e stateChanged)
    DownloadTask* m_dl   = nullptr;   // cast: caminho byte-segmento (inalterado)
    TorrentTask*  m_tor  = nullptr;   // cast: caminho peça->célula (Task 14)
    QTimer        m_repaint;
    static constexpr int    kCellPx     = 9;
    static constexpr qint64 kBlockBytes = 4LL << 20;   // ~4 MiB por tile
    static constexpr int    kMinTiles   = 256;
    static constexpr int    kMaxTiles   = 16384;
};

#pragma once
#include <QWidget>
#include <QTimer>
class DownloadTask;

class ProgressGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProgressGridWidget(QWidget* parent = nullptr);
    void setTask(DownloadTask* t);
    QSize sizeHint() const override { return QSize(240, 160); }        // painel não colapsa
    QSize minimumSizeHint() const override { return QSize(40, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void scheduleRepaint();
    void relayout();          // ajusta a altura do widget p/ o nº de tiles/colunas atuais
    int  cellCount() const;   // total de tiles: fixo pelo tamanho do arquivo
    int  viewportHeight() const;
    DownloadTask* m_task = nullptr;
    QTimer        m_repaint;
    static constexpr int    kCellPx     = 9;
    static constexpr qint64 kBlockBytes = 4LL << 20;   // ~4 MiB por tile
    static constexpr int    kMinTiles   = 256;
    static constexpr int    kMaxTiles   = 16384;
};

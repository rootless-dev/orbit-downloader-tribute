#pragma once
#include <QMainWindow>
#include <QPoint>
#include <QString>
#include <QUrl>
#include <QUuid>
#include "Settings.h"       // AppSettings
#include "Scheduler.h"      // Scheduler, SchedAction
#include "DownloadTypes.h"  // HeaderList
class DownloadManager;
class DownloadTableModel;
class CategoryFilterProxy;
class ProgressGridWidget;
class ClipboardWatcher;
class Logger;
class BrowserBridge;
class QTableView;
class QPlainTextEdit;
class QFormLayout;
class QLabel;
class QDragEnterEvent;
class QDropEvent;
class QActionGroup;
class QTimer;
class QModelIndex;
class QSystemTrayIcon;
enum class DownloadState;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(DownloadManager* mgr, DownloadTableModel* model,
               Logger* logger, QWidget* parent = nullptr);
    void applySettings(const AppSettings& s, const QString& settingsPath);
    // Hooks de teste (sem depender de exec()/UI):
    QString       defaultDirForTest() const { return defaultDir(); }
    ClipboardMode clipModeForTest() const;
    void          applySchedActionForTest(SchedAction a) { routeSchedAction(a); }
    void          setClipboardModeForTest(ClipboardMode m) { setClipboardMode(m); }
    void          applySchedulerConfigForTest(const SchedulerConfig& sc);
    QString       logTextForTest() const;   // definido no .cpp (QPlainTextEdit só forward-declared aqui)
    void          clearCompletedForTest() { clearCompleted(); }
    void          emitBrowserDownloadForTest(const QUrl& url, const HeaderList& headers,
                                             const QString& filename) {
        onBrowserDownload(url, headers, filename);
    }
    void          applyBrowserBridgeForTest(const BrowserPrefs& b) { applyBrowserBridge(b); }
    bool          bridgeListeningForTest() const;
protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
private slots:
    void onNew();
    void onStart();
    void onPause();
    void onDelete();
    void onSelectionChanged();
    void onStateChanged(const QUuid& id, int state);
    void onClipboardUrl(const QUrl& url);
    void onCredentialsRequired(const QUuid& id, const QString& host);
    void onPreferences();
    void onScheduler();
    void onCopyUrl();
    void onAbout();
    void onLogLine(const QUuid& id, const QString& line);
    void onTableContextMenu(const QPoint& pos);
    void onCancel();
    void onMove();
    void onOpen();
    void onOpenFolder();
    void onItemDoubleClicked(const QModelIndex& ix);
    void onBrowserDownload(const QUrl& url, const HeaderList& headers, const QString& filename);
private:
    void    applySchedulerConfig(const SchedulerConfig& sc);
    void    applyBrowserBridge(const BrowserPrefs& b);
    QUuid   selectedId() const;
    QString defaultDir() const;
    void    addUrlViaDialog(const QUrl& prefill);
    void    enqueue(const QUrl& url, const QString& dir);
    void    showLinkNotification(const QUrl& url);
    void    clearLinkNotification();
    void    routeSchedAction(SchedAction a);
    void    maybeQuitWhenDone();
    void    setClipboardMode(ClipboardMode m);
    void    clearCompleted();
    DownloadManager*     m_mgr;
    DownloadTableModel*  m_model;
    CategoryFilterProxy* m_proxy;
    QTableView*          m_table;
    ProgressGridWidget*  m_grid;
    QPlainTextEdit*      m_log;
    QLabel*              m_props;
    ClipboardWatcher*    m_clip;
    QLabel*              m_notice = nullptr;   // notificação clicável do modo Notify (ou nullptr)
    QString              m_lastDir;   // pasta padrão: última escolhida NESTA sessão (spec §3.7)
    AppSettings           m_settings;
    QString               m_settingsPath;
    QActionGroup*         m_clipGroup = nullptr;   // p/ refletir o modo persistido no menu
    Scheduler             m_scheduler;
    QTimer*               m_schedTimer = nullptr;
    Logger*  m_logger = nullptr;
    QUuid    m_logShownId;     // download cujo log está na aba
    QSystemTrayIcon* m_tray = nullptr;
    QString          m_lastCompletedPath;   // último concluído: alvo do clique na notificação
    BrowserBridge*   m_bridge = nullptr;
};

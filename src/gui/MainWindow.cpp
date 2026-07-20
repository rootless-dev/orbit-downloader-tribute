#include "MainWindow.h"
#include "DownloadManager.h"
#include "DownloadTask.h"
#include "DownloadTableModel.h"
#include "CategoryFilterProxy.h"
#include "CategoryTree.h"
#include "ProgressGridWidget.h"
#include "NewDownloadDialog.h"
#include "DropTargets.h"
#include "ClipboardWatcher.h"
#include "CredentialsDialog.h"
#include "PreferencesDialog.h"
#include "SchedulerDialog.h"
#include "UrlName.h"
#include "Logger.h"
#include "ContextMenuRules.h"
#include "BrowserBridge.h"
#include "BrowserBridgeProtocol.h"   // kExtensionOrigin
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QScrollArea>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableView>
#include <QTime>
#include <QTimer>
#include <QToolBar>

MainWindow::MainWindow(DownloadManager* mgr, DownloadTableModel* model,
                       Logger* logger, QWidget* parent)
    : QMainWindow(parent), m_mgr(mgr), m_model(model), m_logger(logger) {
    setAcceptDrops(true);
    // Toolbar
    auto* tb = addToolBar("Main");
    auto* aNew    = tb->addAction("New");
    auto* aStart  = tb->addAction("Start");
    auto* aPause  = tb->addAction("Pause");
    auto* aDelete = tb->addAction("Delete");
    tb->addSeparator();
    auto* aPauseAll  = tb->addAction("Pause All");
    auto* aResumeAll = tb->addAction("Resume All");
    auto* aSched     = tb->addAction("Scheduler");
    auto* aPrefs     = tb->addAction("Preferences");
    connect(aNew,       &QAction::triggered, this, &MainWindow::onNew);
    connect(aStart,     &QAction::triggered, this, &MainWindow::onStart);
    connect(aPause,     &QAction::triggered, this, &MainWindow::onPause);
    connect(aDelete,    &QAction::triggered, this, &MainWindow::onDelete);
    connect(aPauseAll,  &QAction::triggered, this, [this]{ m_mgr->pauseAll(); });
    connect(aResumeAll, &QAction::triggered, this, [this]{ m_mgr->resumeAll(); });
    connect(aSched,     &QAction::triggered, this, &MainWindow::onScheduler);
    connect(aPrefs,     &QAction::triggered, this, &MainWindow::onPreferences);

    // Left tree + proxy + table
    // NOTE (API divergence from brief): CategoryFilterProxy declares no
    // explicit constructor, so it only has the implicit default ctor (no
    // parent param) - QSortFilterProxyModel's QObject*-parent constructor is
    // not inherited. Construct with no parent, then setParent() explicitly
    // for ownership/cleanup.
    m_proxy = new CategoryFilterProxy();
    m_proxy->setParent(this);
    m_proxy->setSourceModel(m_model);
    auto* tree = new CategoryTree(this);
    connect(tree, &CategoryTree::filterChanged, this,
            [this](CategoryFilterProxy::Filter f){ m_proxy->setFilter(f); });

    m_table = new QTableView(this);
    m_table->setModel(m_proxy);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &MainWindow::onSelectionChanged);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested,
            this, &MainWindow::onTableContextMenu);
    connect(m_table, &QTableView::doubleClicked,
            this, &MainWindow::onItemDoubleClicked);

    // Bottom tabs
    m_grid  = new ProgressGridWidget;   // ownership passa ao QScrollArea abaixo
    m_log   = new QPlainTextEdit(this); m_log->setReadOnly(true);
    if (m_logger)
        connect(m_logger, &Logger::lineLogged, this, &MainWindow::onLogLine);
    m_props = new QLabel("—");           // ownership passa ao QScrollArea abaixo
    m_props->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_props->setWordWrap(true);
    m_props->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // A grade vive num scroll: colunas acompanham a largura; quando as tiles
    // exigem mais linhas do que cabe na altura, aparece a barra vertical.
    auto* gridScroll = new QScrollArea(this);
    gridScroll->setWidget(m_grid);
    gridScroll->setWidgetResizable(true);
    gridScroll->setFrameShape(QFrame::NoFrame);
    gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Properties também num scroll: sem isso, uma URL/caminho longo dá ao QLabel
    // um sizeHint larguíssimo e o QTabWidget força a JANELA a ficar larga
    // (o mínimo só era recalculado ao trocar de aba — daí a janela "explodir"
    // ao abrir Properties/Progress). Com word-wrap + scroll, o conteúdo nunca
    // empurra a largura da janela.
    auto* propScroll = new QScrollArea(this);
    propScroll->setWidget(m_props);
    propScroll->setWidgetResizable(true);
    propScroll->setFrameShape(QFrame::NoFrame);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(m_log,      "Log");
    tabs->addTab(gridScroll, "Progress");
    tabs->addTab(propScroll, "Properties");

    // Layout: [tree | table] over tabs
    auto* hsplit = new QSplitter(Qt::Horizontal, this);
    hsplit->addWidget(tree);
    hsplit->addWidget(m_table);
    hsplit->setStretchFactor(1, 1);
    auto* vsplit = new QSplitter(Qt::Vertical, this);
    vsplit->addWidget(hsplit);
    vsplit->addWidget(tabs);
    vsplit->setStretchFactor(0, 1);
    setCentralWidget(vsplit);

    connect(m_mgr, &DownloadManager::taskStateChanged, this,
        [this](const QUuid& id, DownloadState s){ onStateChanged(id, int(s)); });

    connect(m_mgr, &DownloadManager::credentialsRequired,
            this, &MainWindow::onCredentialsRequired);

    // Notificação de conclusão (spec): bandeja do sistema; clique abre o
    // último arquivo concluído. QSystemTrayIcon existe mesmo sem bandeja
    // visível (ex.: plataforma "offscreen" dos testes) — show()/showMessage()
    // são no-ops nesse caso, sem crashar.
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(qApp->windowIcon());
    m_tray->show();
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]{
        if (!m_lastCompletedPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastCompletedPath));
    });

    m_clip = new ClipboardWatcher(this);
    connect(m_clip, &ClipboardWatcher::urlDetected, this, &MainWindow::onClipboardUrl);

    // ---- Barra de menus completa (nativa: no macOS os menus vão para o topo da
    // tela e Preferences/Quit/About são realocados para o menu do app pelo Qt).
    // As ações do toolbar são reusadas aqui (mesma QAction no toolbar e no menu).
    auto* mb = menuBar();

    // File
    QMenu* file = mb->addMenu(tr("&File"));
    aNew->setShortcut(QKeySequence::New);
    file->addAction(aNew);                        // reusa "New" do toolbar
    QAction* aOpenFolder = file->addAction(tr("Open downloads folder"));
    connect(aOpenFolder, &QAction::triggered, this, [this]{
        QDesktopServices::openUrl(QUrl::fromLocalFile(defaultDir()));
    });
    file->addSeparator();
    QAction* aQuit = file->addAction(tr("Quit"));
    aQuit->setShortcut(QKeySequence::Quit);
    aQuit->setMenuRole(QAction::QuitRole);        // -> menu do app no macOS
    connect(aQuit, &QAction::triggered, this, []{ qApp->quit(); });

    // Edit
    QMenu* edit = mb->addMenu(tr("&Edit"));
    aDelete->setShortcut(QKeySequence::Delete);
    edit->addAction(aDelete);                     // reusa "Delete" do toolbar
    QAction* aSelectAll = edit->addAction(tr("Select All"));
    aSelectAll->setShortcut(QKeySequence::SelectAll);
    connect(aSelectAll, &QAction::triggered, this, [this]{ m_table->selectAll(); });
    QAction* aCopyUrl = edit->addAction(tr("Copy URL"));
    aCopyUrl->setShortcut(QKeySequence::Copy);
    connect(aCopyUrl, &QAction::triggered, this, &MainWindow::onCopyUrl);
    QAction* aClearDone = edit->addAction(tr("Clear Completed"));
    connect(aClearDone, &QAction::triggered, this, &MainWindow::clearCompleted);

    // View
    QMenu* view = mb->addMenu(tr("&View"));
    QAction* aShowToolbar = view->addAction(tr("Show Toolbar"));
    aShowToolbar->setCheckable(true); aShowToolbar->setChecked(true);
    connect(aShowToolbar, &QAction::toggled, tb, &QToolBar::setVisible);
    QAction* aShowCats = view->addAction(tr("Show Categories"));
    aShowCats->setCheckable(true); aShowCats->setChecked(true);
    connect(aShowCats, &QAction::toggled, tree, &QWidget::setVisible);

    // Tools (mantém o submenu Clipboard monitor existente + Scheduler/Preferences)
    QMenu* tools = mb->addMenu(tr("&Tools"));
    QMenu* clip  = tools->addMenu(tr("&Clipboard monitor"));
    auto*  group = new QActionGroup(this);
    group->setExclusive(true);
    m_clipGroup = group;

    struct { const char* label; ClipboardMode mode; } items[] = {
        { QT_TR_NOOP("Off"),                   ClipboardMode::Off    },
        { QT_TR_NOOP("Ask (open New dialog)"), ClipboardMode::Ask    },
        { QT_TR_NOOP("Add automatically"),     ClipboardMode::Auto   },
        { QT_TR_NOOP("Notify in status bar"),  ClipboardMode::Notify },
    };
    for (const auto& it : items) {
        QAction* a = clip->addAction(tr(it.label));
        a->setCheckable(true);
        a->setChecked(it.mode == ClipboardMode::Off);      // padrão
        a->setData(int(it.mode));   // p/ applySettings()/onPreferences() marcarem o rádio certo
        group->addAction(a);
        const ClipboardMode m = it.mode;
        connect(a, &QAction::triggered, this, [this, m] { setClipboardMode(m); });
    }
    tools->addSeparator();
    QAction* aAppLog = tools->addAction(tr("Application Log"));
    connect(aAppLog, &QAction::triggered, this, [this]{
        if (m_logger)
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_logger->logsDir()));
    });
    tools->addSeparator();
    tools->addAction(aSched);                     // reusa "Scheduler" do toolbar
    aPrefs->setShortcut(QKeySequence::Preferences);
    aPrefs->setMenuRole(QAction::PreferencesRole);   // -> menu do app no macOS
    tools->addAction(aPrefs);                     // reusa "Preferences" do toolbar

    // Help
    QMenu* help = mb->addMenu(tr("&Help"));
    QAction* aAbout = help->addAction(tr("About"));
    aAbout->setMenuRole(QAction::AboutRole);      // -> menu do app no macOS
    connect(aAbout, &QAction::triggered, this, &MainWindow::onAbout);
}

QUuid MainWindow::selectedId() const {
    const QModelIndex cur = m_table->currentIndex();
    if (!cur.isValid()) return {};
    const QModelIndex src = m_proxy->mapToSource(cur);
    DownloadTask* t = m_model->taskAt(src.row());
    return t ? t->id() : QUuid();
}

void MainWindow::onNew() { addUrlViaDialog(QUrl()); }

// Pasta padrão (drop múltiplo): última escolhida no New nesta sessão; senão a
// pasta persistida nas Preferences; senão Downloads na primeira vez.
QString MainWindow::defaultDir() const {
    if (!m_lastDir.isEmpty()) return m_lastDir;
    if (!m_settings.ui.defaultDownloadDir.isEmpty()) return m_settings.ui.defaultDownloadDir;
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void MainWindow::addUrlViaDialog(const QUrl& prefill) {
    NewDownloadDialog d(this, prefill);
    if (d.exec() != QDialog::Accepted) return;
    // A pasta escolhida vira a padrão desta sessão (drop múltiplo usa).
    m_lastDir = QFileInfo(d.destPath()).absolutePath();
    const QUuid id = m_mgr->addDownload(d.url(), d.destPath());
    m_model->appendTask(m_mgr->taskById(id));
}

void MainWindow::enqueue(const QUrl& url, const QString& dir) {
    // provisionalName=true: o nome saiu da URL (palpite) — o motor adota o
    // Content-Disposition do servidor se houver (clipboard/drag).
    const QUuid id = m_mgr->addDownload(url, QDir(dir).filePath(deriveFileName(url)), {}, true);
    m_model->appendTask(m_mgr->taskById(id));   // UrlName.h (Fase 2)
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!extractUrls(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = extractUrls(e->mimeData());
    if (urls.isEmpty()) return;               // rejeitado, sem diálogo de erro
    e->acceptProposedAction();

    // Uma URL: o New pré-preenchido, p/ escolher pasta. Várias: perguntar N
    // vezes seria insuportável — vão todas p/ a pasta padrão (spec §3.7).
    if (urls.size() == 1) { addUrlViaDialog(urls.first()); return; }
    for (const QUrl& u : urls) enqueue(u, defaultDir());
}

void MainWindow::onStart()  { const QUuid id = selectedId(); if (!id.isNull()) m_mgr->resume(id); }
void MainWindow::onPause()  { const QUuid id = selectedId(); if (!id.isNull()) m_mgr->pause(id); }

// Delete com opção de apagar os arquivos do disco (spec §3.4): checkbox no
// próprio QMessageBox em vez de um segundo diálogo de confirmação.
void MainWindow::onDelete() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    QMessageBox box(this);
    box.setWindowTitle(tr("Delete"));
    box.setText(tr("Remove this download?"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    QCheckBox* chk = new QCheckBox(tr("Also delete the files from disk"), &box);
    box.setCheckBox(chk);
    if (box.exec() != QMessageBox::Yes) return;
    m_mgr->remove(id, /*deleteFiles=*/chk->isChecked());
    m_model->removeTaskById(id);
}

// Menu de contexto da tabela (spec §3.4): monta as ações inline, habilitando
// cada uma conforme o estado da task clicada (regras puras em
// ContextMenuRules.h), e opera sobre a LINHA CLICADA (não a que já estava
// selecionada) - por isso setCurrentIndex() antes de ler selectedId().
void MainWindow::onTableContextMenu(const QPoint& pos) {
    const QModelIndex ix = m_table->indexAt(pos);
    if (!ix.isValid()) return;
    m_table->setCurrentIndex(ix);                 // seleção reflete a linha clicada;
                                                  // selectedId() (usado pelos handlers) segue-a
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    const DownloadState s = t->state();

    QMenu menu(this);
    QAction* aStart  = menu.addAction(tr("Start"));
    QAction* aStop   = menu.addAction(tr("Stop"));
    QAction* aCancel = menu.addAction(tr("Cancel"));
    menu.addSeparator();
    QAction* aDelete = menu.addAction(tr("Delete..."));
    QAction* aMove   = menu.addAction(tr("Move..."));
    menu.addSeparator();
    QAction* aOpen   = menu.addAction(tr("Open"));
    QAction* aFolder = menu.addAction(tr("Open containing folder"));
    menu.addSeparator();
    QMenu* prio = menu.addMenu(tr("Priority"));
    QAction* pHigh = prio->addAction(tr("High"));
    QAction* pNorm = prio->addAction(tr("Normal"));
    QAction* pLow  = prio->addAction(tr("Low"));
    for (auto* a : {pHigh, pNorm, pLow}) a->setCheckable(true);
    pHigh->setChecked(t->priority() == Priority::High);
    pNorm->setChecked(t->priority() == Priority::Normal);
    pLow ->setChecked(t->priority() == Priority::Low);

    aStart->setEnabled(ctxCanStart(s));
    aStop->setEnabled(ctxCanStop(s));
    aCancel->setEnabled(ctxCanCancel(s));
    aMove->setEnabled(ctxCanMove(s));
    aOpen->setEnabled(ctxCanOpen(s));

    connect(aStart,  &QAction::triggered, this, &MainWindow::onStart);
    connect(aStop,   &QAction::triggered, this, &MainWindow::onPause);
    connect(aCancel, &QAction::triggered, this, &MainWindow::onCancel);
    connect(aDelete, &QAction::triggered, this, &MainWindow::onDelete);
    connect(aMove,   &QAction::triggered, this, &MainWindow::onMove);
    connect(aOpen,   &QAction::triggered, this, &MainWindow::onOpen);
    connect(aFolder, &QAction::triggered, this, &MainWindow::onOpenFolder);
    connect(pHigh, &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::High);   m_model->refreshRow(id); });
    connect(pNorm, &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::Normal); m_model->refreshRow(id); });
    connect(pLow,  &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::Low);    m_model->refreshRow(id); });

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::onCancel() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    const auto btn = QMessageBox::question(this, tr("Cancel"),
        tr("Cancel this download? The partial file will be discarded."));
    if (btn == QMessageBox::Yes) m_mgr->cancel(id);
}

void MainWindow::onMove() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Move to folder"));
    if (dir.isEmpty()) return;
    if (!m_mgr->moveFiles(id, dir))
        QMessageBox::warning(this, tr("Move"), tr("Could not move the files."));
}

void MainWindow::onOpen() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (t) QDesktopServices::openUrl(QUrl::fromLocalFile(t->record().destPath));
}

void MainWindow::onOpenFolder() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    const QString path = t->record().destPath;
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {"-R", path});     // revela e seleciona no Finder
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

// Double-click (spec): concluído -> abre o arquivo; parado/pausado/erro/
// cancelado (ctxCanStart) -> inicia; ativo (Connecting/Downloading) -> no-op.
void MainWindow::onItemDoubleClicked(const QModelIndex&) {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Completed) onOpen();
    else if (ctxCanStart(t->state()))           onStart();
    // ativo: no-op
}

// Edit -> Clear Completed (spec): remove só os concluídos, SEM apagar os
// arquivos do disco. Coleta os ids primeiro para não mutar m_mgr->tasks()
// enquanto itera.
void MainWindow::clearCompleted() {
    QVector<QUuid> done;
    for (DownloadTask* t : m_mgr->tasks())
        if (t->state() == DownloadState::Completed) done.append(t->id());
    for (const QUuid& id : done) {
        m_mgr->remove(id, /*deleteFiles=*/false);
        m_model->removeTaskById(id);
    }
    if (m_logger)
        m_logger->logApp(LogLevel::Info, QString("Cleared %1 completed download(s)").arg(done.size()));
}

void MainWindow::onSelectionChanged() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    m_grid->setTask(t);
    if (!t) {
        m_props->setText("—");
    } else {
        const auto r = t->record();
        m_props->setText(QString("URL: %1\nDest: %2\nSize: %3\nSegments: %4\nRange: %5")
            .arg(r.url.toString(), r.destPath)
            .arg(r.totalBytes).arg(r.segmentCount).arg(r.supportsRange ? "yes" : "no"));
    }

    // Log por-download: recarrega o arquivo do item selecionado e passa a
    // anexar as novas linhas dele (ver onLogLine).
    m_logShownId = id;
    m_log->clear();
    if (t && m_logger) {
        QFile f(m_logger->taskLogPath(id, t->record().destPath));
        if (f.open(QIODevice::ReadOnly))
            m_log->setPlainText(QString::fromUtf8(f.readAll()));
    }
}

void MainWindow::onStateChanged(const QUuid& id, int state) {
    DownloadTask* t = m_mgr->taskById(id);
    const QString name = t ? QFileInfo(t->record().destPath).fileName() : id.toString();
    static const char* names[] = {"Queued","Connecting","Downloading","Paused",
                                  "Completed","Error","Cancelled"};
    if (m_logger)
        m_logger->logApp(state == int(DownloadState::Error) ? LogLevel::Error : LogLevel::Info,
                         QString("%1 -> %2").arg(name, names[state]));
    if (state == int(DownloadState::Completed) && t && m_tray) {
        m_lastCompletedPath = t->record().destPath;
        m_tray->showMessage(tr("Download complete"), name,
                            QSystemTrayIcon::Information, 5000);
    }
    maybeQuitWhenDone();
}

void MainWindow::onLogLine(const QUuid& id, const QString& line) {
    if (id == m_logShownId && !id.isNull())
        m_log->appendPlainText(line);
}

void MainWindow::onClipboardUrl(const QUrl& url) {
    switch (m_clip->mode()) {
        case ClipboardMode::Off:
            return;
        case ClipboardMode::Ask:
            addUrlViaDialog(url);
            return;
        case ClipboardMode::Auto:
            enqueue(url, defaultDir());
            return;
        case ClipboardMode::Notify:
            showLinkNotification(url);
            return;
    }
}

void MainWindow::onCredentialsRequired(const QUuid& id, const QString& host) {
    CredentialsDialog d(host, this);
    if (d.exec() != QDialog::Accepted) return;   // cancelou: a task fica Paused,
                                                 // o botão Start tenta de novo (spec §3.6)
    m_mgr->provideCredentials(id, d.user(), d.pass());
}

// Notificação CLICÁVEL na barra de status (spec §3.7). QStatusBar::showMessage
// só mostra texto morto — não serve. Um QLabel com rich text emite linkActivated
// quando clicado; ele se remove sozinho após 8s ou ao ser usado.
void MainWindow::showLinkNotification(const QUrl& url) {
    if (m_notice) { statusBar()->removeWidget(m_notice); m_notice->deleteLater(); }

    m_notice = new QLabel(tr("Link detected: <a href=\"#\">%1</a>")
                              .arg(url.toString().toHtmlEscaped()), this);
    m_notice->setTextFormat(Qt::RichText);
    statusBar()->addWidget(m_notice);
    m_notice->show();

    connect(m_notice, &QLabel::linkActivated, this, [this, url] {
        clearLinkNotification();
        addUrlViaDialog(url);
    });
    QTimer::singleShot(8000, this, &MainWindow::clearLinkNotification);
}

void MainWindow::clearLinkNotification() {
    if (!m_notice) return;
    statusBar()->removeWidget(m_notice);
    m_notice->deleteLater();
    m_notice = nullptr;
}

ClipboardMode MainWindow::clipModeForTest() const { return m_clip->mode(); }
QString MainWindow::logTextForTest() const { return m_log->toPlainText(); }

// Centraliza a troca de modo do clipboard: aplica no watcher e persiste em
// settings.json (Task 13, Fase 4) para que a escolha via menu Tools sobreviva
// a um restart.
void MainWindow::setClipboardMode(ClipboardMode m) {
    m_clip->setMode(m);
    m_settings.ui.clipboardMode = m;
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}

// Aplica settings carregadas/persistidas: modo do clipboard (+ rádio do menu
// Tools refletindo o valor) e pasta padrão de download (spec §3.7, Fase 4).
void MainWindow::applySettings(const AppSettings& s, const QString& settingsPath) {
    m_settings     = s;
    m_settingsPath = settingsPath;
    m_clip->setMode(s.ui.clipboardMode);
    if (m_clipGroup) {
        for (QAction* a : m_clipGroup->actions())
            if (a->data().toInt() == int(s.ui.clipboardMode)) { a->setChecked(true); break; }
    }
    if (!s.ui.defaultDownloadDir.isEmpty()) m_lastDir = s.ui.defaultDownloadDir;

    applySchedulerConfig(s.scheduler);
    applyBrowserBridge(s.browser);
}

// Recebe um download solicitado pela extensão do browser (Task 6/8, Fase 5):
// deriva o nome do arquivo (sugerido pela extensão ou pela própria URL),
// enfileira com os headers encaminhados (cookies/referer/UA) e notifica via
// bandeja. addDownload() devolve QUuid nulo p/ esquema não suportado — nesse
// caso não há task pra anexar ao model.
void MainWindow::onBrowserDownload(const QUrl& url, const HeaderList& headers,
                                   const QString& filename) {
    // Endurecimento (revisão final, Fase 5): o filename vem do browser (não
    // confiável) — reduz a um basename antes de juntar ao diretório de
    // destino, senão um path absoluto ou "../" poderia escapar do dir.
    const QString base = filename.isEmpty() ? QString() : QFileInfo(filename).fileName();
    const QString name = base.isEmpty() ? deriveFileName(url) : base;
    const QString dest = QDir(defaultDir()).filePath(name);
    // provisionalName=true: o nome veio do browser/URL (palpite) — o motor
    // adota o Content-Disposition do servidor se houver (corrige, p.ex., URLs
    // do Google Drive cujo path é "/download").
    const QUuid id = m_mgr->addDownload(url, dest, headers, /*provisionalName=*/true);
    if (id.isNull()) return;                     // esquema não suportado
    m_model->appendTask(m_mgr->taskById(id));
    if (m_tray)
        m_tray->showMessage(tr("Orbit"), tr("New download from browser: %1").arg(name),
                            QSystemTrayIcon::Information, 5000);
}

// (Re)aplica a configuração do bridge do browser (Task 8, Fase 5): cria o
// BrowserBridge uma única vez (conecta o sinal uma única vez), sempre para
// antes de reaplicar (idempotente ao trocar porta/token/enabled), e só volta
// a ouvir se habilitado com token não vazio. Porta ocupada (spec §7): NÃO
// trava a app — só avisa via bandeja e segue sem o bridge ativo.
void MainWindow::applyBrowserBridge(const BrowserPrefs& b) {
    if (!m_bridge) {
        m_bridge = new BrowserBridge(this);
        connect(m_bridge, &BrowserBridge::downloadRequested,
                this, &MainWindow::onBrowserDownload);
    }
    m_bridge->stop();
    if (b.enabled && !b.token.isEmpty()) {
        if (!m_bridge->start(b.port, b.token, kExtensionOrigin) && m_tray) {
            m_tray->showMessage(tr("Orbit"),
                tr("Browser bridge could not bind port %1 (in use).").arg(b.port),
                QSystemTrayIcon::Warning, 5000);
        }
    }
}

// Hook de teste: estado atual do bridge (Task 8, Fase 5 — fix de reaplicação
// ao mudar Preferences). Sem bridge criado ainda, não está ouvindo.
bool MainWindow::bridgeListeningForTest() const {
    return m_bridge && m_bridge->listening();
}

// Centraliza a aplicação da config do scheduler: guarda em m_settings, arma o
// Scheduler (setConfig re-arma o estado de disparo — Task 15), garante o timer
// de 30s (criado uma única vez) e faz um tick imediato para que uma janela já
// ativa comece sem esperar o próximo tick periódico. NÃO persiste — quem
// persiste é onScheduler()/applySchedulerConfigForTest() (boot via
// applySettings() não deve reescrever settings.json).
void MainWindow::applySchedulerConfig(const SchedulerConfig& sc) {
    m_settings.scheduler = sc;
    m_scheduler.setConfig(sc);
    if (!m_schedTimer) {
        m_schedTimer = new QTimer(this);
        connect(m_schedTimer, &QTimer::timeout, this, [this]{
            routeSchedAction(m_scheduler.tick(QDateTime::currentDateTime()));
        });
    }
    m_schedTimer->start(30'000);   // tick a cada 30s
    routeSchedAction(m_scheduler.tick(QDateTime::currentDateTime()));  // aplica já (não espera 30s)
}

void MainWindow::onScheduler() {
    SchedulerDialog dlg(m_settings.scheduler, this);
    if (dlg.exec() != QDialog::Accepted) return;
    applySchedulerConfig(dlg.result());
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}

void MainWindow::applySchedulerConfigForTest(const SchedulerConfig& sc) {
    applySchedulerConfig(sc);
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}

void MainWindow::routeSchedAction(SchedAction a) {
    if (a == SchedAction::StartAll) m_mgr->resumeAll();
    else if (a == SchedAction::StopAll) m_mgr->pauseAll();
}

void MainWindow::maybeQuitWhenDone() {
    if (!m_settings.scheduler.quitWhenDone) return;
    const auto tasks = m_mgr->tasks();
    if (tasks.isEmpty()) return;                       // não fecha app vazio
    for (DownloadTask* t : tasks)
        if (t->state() != DownloadState::Completed) return;
    qApp->quit();
}

void MainWindow::onPreferences() {
    PreferencesDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_settings = dlg.result();
    m_mgr->setConfig(m_settings.engine);
    applyBrowserBridge(m_settings.browser);
    m_clip->setMode(m_settings.ui.clipboardMode);
    if (m_clipGroup) {
        for (QAction* a : m_clipGroup->actions())
            if (a->data().toInt() == int(m_settings.ui.clipboardMode)) { a->setChecked(true); break; }
    }
    if (!m_settings.ui.defaultDownloadDir.isEmpty()) m_lastDir = m_settings.ui.defaultDownloadDir;
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}

void MainWindow::onCopyUrl() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    DownloadTask* t = m_mgr->taskById(id);
    if (!t) return;
    if (m_clip) m_clip->markSelfCopy();           // não re-oferecer o que a app copiou
    QApplication::clipboard()->setText(t->record().url.toString());
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About Orbit Downloader Tribute"),
        tr("Orbit Downloader Tribute\n\n"
           "Reimplementação em C++20 / Qt 6 do clássico Orbit Downloader.\n"
           "Tributo/reimplementação independente — não afiliado ao Orbit original."));
}

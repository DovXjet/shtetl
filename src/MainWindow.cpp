#include "MainWindow.h"

#include "ModelFormats.h"
#include "Logging.h"
#include "NavigationStyles.h"
#include "OpenFileDialog.h"
#include "ThumbnailBrowser.h"
#include "ThumbnailService.h"
#include "Viewport3D.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QProgressBar>
#include <QShortcut>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace
{
  constexpr int MaxRecentFiles = 8;
} // namespace

MainWindow::MainWindow()
{
  resize(1280, 800);
  setWindowTitle("shtetl");

  m_Viewport = new Viewport3D(this);
  setCentralWidget(m_Viewport);

  // The thumbnailer renders on the viewport's device, which exists now.
  m_Service = new ThumbnailService(this);
  m_Service->initialize(m_Viewport->device(), m_Viewport->options());

  m_Browser = new ThumbnailBrowser(m_Service, this);
  m_BrowserDock = new QDockWidget("Models", this);
  m_BrowserDock->setObjectName("ModelsDock");
  m_BrowserDock->setWidget(m_Browser);
  addDockWidget(Qt::LeftDockWidgetArea, m_BrowserDock);
  resizeDocks({m_BrowserDock}, {380}, Qt::Horizontal);

  connect(m_Browser, &ThumbnailBrowser::fileSelected, this, &MainWindow::loadModel);

  // Right-click in the 3D view: the Edit > Copy action (set up in createMenus()).
  connect(m_Viewport, &Viewport3D::contextMenuRequested, this, [this](const QPoint& pos) {
    QMenu menu(this);
    menu.addAction(m_CopyAction);
    menu.exec(pos);
  });

  // The progress of a model being loaded in the background; it shows up only
  // when the load takes a while, and Escape drops the load.
  m_LoadPool.setMaxThreadCount(2); // the one dropped may still be winding down
  m_Progress = new QProgressBar(this);
  m_Progress->setFixedWidth(180);
  m_Progress->setVisible(false);
  statusBar()->addPermanentWidget(m_Progress);
  m_ProgressTimer.setInterval(50);
  connect(&m_ProgressTimer, &QTimer::timeout, this, &MainWindow::updateLoadProgress);
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  connect(escape, &QShortcut::activated, this, &MainWindow::cancelLoad);

  m_ReloadTimer.setSingleShot(true);
  m_ReloadTimer.setInterval(400);
  connect(&m_ReloadTimer, &QTimer::timeout, this, &MainWindow::autoReload);
  connect(&m_Watcher, &QFileSystemWatcher::fileChanged, &m_ReloadTimer, qOverload<>(&QTimer::start));
  connect(&m_Watcher, &QFileSystemWatcher::directoryChanged, &m_ReloadTimer, qOverload<>(&QTimer::start));

  // The mouse side buttons are caught for the whole application, as the 3D
  // view is a native window that vsgQt reads its own events from.
  qApp->installEventFilter(this);

  const QSettings settings;
  // Window size/position and the dock's width; the dock's visibility is
  // applied by createMenus() from its own key.
  restoreGeometry(settings.value("Window/Geometry").toByteArray());
  restoreState(settings.value("Window/State").toByteArray());

  createMenus();
  statusBar()->showMessage("Open a model or a directory");

  showDirectory(settings.value("lastDirectory", QDir::homePath()).toString());
}

MainWindow::~MainWindow()
{
  qApp->removeEventFilter(this);
  if (m_Load.cancel) m_Load.cancel->store(true);
  m_LoadPool.waitForDone();
  // Stops the thumbnail thread, which renders on the viewport's device,
  // before the viewport (and the device) is destroyed.
  delete m_Service;
  m_Service = nullptr;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  QSettings settings;
  settings.setValue("Window/Geometry", saveGeometry());
  settings.setValue("Window/State", saveState());
  QMainWindow::closeEvent(event);
}

// The mouse's Forward and Back (side) buttons step to the next and previous
// model, wherever in the window the pointer is.
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
  const auto type = event->type();
  if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) && QApplication::activeWindow() == this)
  {
    const Qt::MouseButton button = static_cast<QMouseEvent*>(event)->button();
    if (button == Qt::BackButton || button == Qt::ForwardButton)
    {
      if (type == QEvent::MouseButtonPress) goAdjacent(button == Qt::ForwardButton ? +1 : -1);
      return true; // the trackball must not see them
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::createMenus()
{
  // See vgiv: with no consumer for a global menu the bar would vanish.
  menuBar()->setNativeMenuBar(false);

  auto* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction("&Open...", QKeySequence("Ctrl+O"), this, &MainWindow::openFileDialog);
  fileMenu->addAction("Open &Directory...", QKeySequence("Ctrl+Shift+O"), this, &MainWindow::openDirectoryDialog);
  m_RecentMenu = fileMenu->addMenu("Recent Files");
  updateRecentMenu();
  fileMenu->addSeparator();
  fileMenu->addAction("&Reload", QKeySequence("F5"), this, &MainWindow::reload);
  fileMenu->addSeparator();
  auto* quit = fileMenu->addAction("&Quit", qApp, &QApplication::quit);
  quit->setShortcuts({QKeySequence::Quit, QKeySequence("Q")});

  // Copy puts the model file on the system clipboard, as a file manager does,
  // so that it can be pasted into XjetStudio (as a part), a file manager, ...
  auto* editMenu = menuBar()->addMenu("&Edit");
  m_CopyAction = editMenu->addAction("&Copy", QKeySequence::Copy, this, &MainWindow::copyToClipboard);
  m_CopyAction->setStatusTip("Copy the model file to the clipboard");
  m_CopyAction->setEnabled(false); // until a model is shown

  auto* viewMenu = menuBar()->addMenu("&View");
  // Also added to the window, so its shortcut works - and can bring the menu
  // back - while the menu bar is hidden.
  // The View checkboxes are remembered between runs.
  QSettings settings;
  auto* menuBarAction = viewMenu->addAction("Menu Bar", this, [this](bool show) {
    menuBar()->setVisible(show);
    QSettings().setValue("View/MenuBar", show);
  });
  menuBarAction->setCheckable(true);
  const bool menuBarShown = settings.value("View/MenuBar", true).toBool();
  menuBarAction->setChecked(menuBarShown);
  menuBar()->setVisible(menuBarShown);
  menuBarAction->setShortcut(QKeySequence("Ctrl+M"));
  addAction(menuBarAction);
  auto* dockAction = m_BrowserDock->toggleViewAction();
  dockAction->setText("Thumbnails");
  dockAction->setShortcut(QKeySequence("F9"));
  viewMenu->addAction(dockAction);
  auto* occlusion = viewMenu->addAction("&Ambient Occlusion");
  occlusion->setCheckable(true);
  occlusion->setShortcut(QKeySequence("A"));
  occlusion->setStatusTip("Darken the creases of STL and 3MF models");
  addAction(occlusion);
  connect(occlusion, &QAction::toggled, this, [this](bool on) {
    m_Viewport->setAmbientOcclusion(on);
    QSettings().setValue("View/AmbientOcclusion", on);
  });
  occlusion->setChecked(QSettings().value("View/AmbientOcclusion", false).toBool());
  auto* autoReload = viewMenu->addAction("Auto Re&load");
  autoReload->setCheckable(true);
  autoReload->setStatusTip("Reload the model when its file changes, e.g. when a CadQuery script exports it");
  connect(autoReload, &QAction::toggled, this, [this](bool on) {
    m_AutoReload = on;
    QSettings().setValue("View/AutoReload", on);
    watchCurrentFile();
  });
  autoReload->setChecked(QSettings().value("View/AutoReload", false).toBool());
  m_BrowserDock->setVisible(settings.value("View/Thumbnails", true).toBool());
  connect(dockAction, &QAction::toggled, this, [](bool show) { QSettings().setValue("View/Thumbnails", show); });
  viewMenu->addSeparator();
  auto* fit = viewMenu->addAction("Fit to Window", m_Viewport, [this]() { m_Viewport->fitAll(); });
  fit->setShortcuts({QKeySequence("Ctrl+0"), QKeySequence("F")});
  auto* ortho = viewMenu->addAction("Orthographic\tNumpad 5");
  ortho->setCheckable(true);
  connect(ortho, &QAction::toggled, m_Viewport, &Viewport3D::setOrthographic);
  connect(m_Viewport, &Viewport3D::projectionChanged, ortho, &QAction::setChecked);
  connect(m_Viewport, &Viewport3D::projectionChanged, this, [](bool on) { QSettings().setValue("View/Orthographic", on); });
  ortho->setChecked(settings.value("View/Orthographic", false).toBool());
  viewMenu->addSeparator();

  // The 3D interaction flavor: the same presets as XjetStudio and RobotMotion,
  // persisted under the same key.
  const NavigationStyle current =
      navigationStyleFromName(settings.value("Navigation/Style", QString::fromStdString(navigationStyleName(NavigationStyle::Netfabb))).toString().toStdString());
  m_Viewport->setNavigationStyle(current);
  auto* styleMenu = viewMenu->addMenu("&Navigation Style");
  auto* group = new QActionGroup(styleMenu);
  for (NavigationStyle style : allNavigationStyles())
  {
    auto* action = styleMenu->addAction(QString::fromStdString(navigationStyleName(style)));
    action->setCheckable(true);
    action->setChecked(style == current);
    action->setToolTip(QString::fromStdString(navigationStyleDescription(style)));
    group->addAction(action);
    connect(action, &QAction::triggered, this, [this, style]() {
      m_Viewport->setNavigationStyle(style);
      QSettings().setValue("Navigation/Style", QString::fromStdString(navigationStyleName(style)));
    });
  }
  styleMenu->setToolTipsVisible(true);
  viewMenu->addSeparator();

  // As vgiv's Next/Previous Image.
  auto* next = viewMenu->addAction("Next Model", this, [this]() { goAdjacent(+1); });
  next->setShortcuts({QKeySequence("Right"), QKeySequence("Shift+Down"), QKeySequence("Space"), QKeySequence::Forward});
  auto* previous = viewMenu->addAction("Previous Model", this, [this]() { goAdjacent(-1); });
  previous->setShortcuts({QKeySequence("Left"), QKeySequence("Shift+Up"), QKeySequence("Backspace"), QKeySequence::Back});

  auto* helpMenu = menuBar()->addMenu("&Help");
  helpMenu->addAction("About shtetl", this, &MainWindow::showAboutDialog);
}

void MainWindow::showAboutDialog()
{
  QDialog dlg(this);
  dlg.setWindowTitle("About shtetl");
  auto* layout = new QVBoxLayout(&dlg);
  layout->setSpacing(8);

  auto addLabel = [&](const QString& text, const QString& style = {}) {
    auto* l = new QLabel(text, &dlg);
    l->setAlignment(Qt::AlignCenter);
    l->setTextFormat(Qt::RichText);
    l->setOpenExternalLinks(true);
    l->setTextInteractionFlags(Qt::TextBrowserInteraction);
    if (!style.isEmpty()) l->setStyleSheet(style);
    layout->addWidget(l);
  };

  auto* logo = new QLabel(&dlg);
  const qreal dpr = dlg.devicePixelRatioF();
  QPixmap pix = QPixmap(":/icons/shtetl-about.png").scaledToHeight(qRound(160 * dpr), Qt::SmoothTransformation);
  pix.setDevicePixelRatio(dpr);
  logo->setPixmap(pix);
  logo->setAlignment(Qt::AlignCenter);
  layout->addWidget(logo);

  addLabel("shtetl " SHTETL_VERSION, "font-size: 24pt; font-weight: bold;");
  addLabel("A simple 3D model viewer with a thumbnail browser");
  addLabel(QString("%1 %2").arg(QSysInfo::currentCpuArchitecture(), QSysInfo::prettyProductName()));
  addLabel("Copyright &copy; Dov Grobgeld, 2026");
  addLabel("Email: <a href=\"mailto:dov.grobgeld@gmail.com\"><code>dov.grobgeld@gmail.com</code></a>");
  addLabel("Commit-Id: " SHTETL_COMMIT_ID);
  addLabel("Commit-Date: " SHTETL_COMMIT_DATE);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  layout->addWidget(buttons);
  dlg.exec();
}

void MainWindow::openPath(const QString& path)
{
  const QFileInfo info(path);
  if (info.isDir())
    showDirectory(info.absoluteFilePath());
  else if (info.isFile())
  {
    showDirectory(info.absolutePath());
    if (!m_Browser->selectFile(info.absoluteFilePath())) // loads it
      loadModel(info.absoluteFilePath()); // not a listed type
  }
  else
  {
    spdlog::error("No such file or directory: {}", path.toStdString());
    statusBar()->showMessage("No such file or directory: " + path);
  }
}

void MainWindow::openFileDialog()
{
  QSettings settings;
  shtetl::OpenFileDialog dialog(this, &settings, m_Service);
  dialog.setDirectory(m_Browser->directory());
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
  openPath(dialog.selectedFiles().first());
}

void MainWindow::openDirectoryDialog()
{
  const QString dir = QFileDialog::getExistingDirectory(this, "Open Directory", m_Browser->directory());
  if (dir.isEmpty()) return;
  showDirectory(dir);
  m_BrowserDock->show();
}

void MainWindow::showDirectory(const QString& path)
{
  m_Browser->setDirectory(path);
  QSettings().setValue("lastDirectory", m_Browser->directory());
}

void MainWindow::loadModel(const QString& path)
{
  if (path.isEmpty()) return;
  // A load still running is dropped: its thread stops at its next check.
  if (m_Load.cancel) m_Load.cancel->store(true);

  m_Load.id++;
  m_Load.running = true;
  m_Load.path = path;
  m_Load.direction = std::exchange(m_StepDirection, 0);
  m_Load.cancel = std::make_shared<std::atomic<bool>>(false);
  m_Load.percent = std::make_shared<std::atomic<int>>(-1);
  m_LoadTime.start();
  m_ProgressTimer.start();
  spdlog::info("Loading {}", path.toStdString());
  statusBar()->showMessage("Loading " + QFileInfo(path).fileName() + "... (Esc cancels)");

  // The loading itself touches no GUI or GPU state, so it can run on its own
  // thread; the result is handed back to this one.
  const quint64 id = m_Load.id;
  const auto cancel = m_Load.cancel;
  const auto percent = m_Load.percent;
  const auto options = m_Viewport->options();
  m_LoadPool.start([this, id, path, cancel, percent, options]() {
    LoadedModel model;
    QString error;
    try
    {
      model = ::loadModel(path, options, cancel.get(), vsg::vec4(0.85f, 0.85f, 0.85f, 1.0f), [percent](int p) { percent->store(p); });
    }
    catch (const std::exception& e)
    {
      error = QString::fromUtf8(e.what());
    }
    catch (const vsg::Exception& e) // not a std::exception
    {
      error = QString::fromStdString(e.message);
    }
    QMetaObject::invokeMethod(this, [this, id, path, model, error]() { onModelLoaded(id, path, model, error); }, Qt::QueuedConnection);
  });
}

void MainWindow::onModelLoaded(quint64 id, const QString& path, LoadedModel model, const QString& error)
{
  if (id != m_Load.id || !m_Load.running) return; // replaced or cancelled meanwhile
  m_Load.running = false;
  m_ProgressTimer.stop();
  m_Progress->setVisible(false);
  const int direction = m_Load.direction;

  QString failure = error;
  if (failure.isEmpty())
  {
    try
    {
      m_Viewport->setModel(model);
    }
    catch (const std::exception& e)
    {
      failure = QString::fromUtf8(e.what());
    }
  }
  if (!failure.isEmpty())
  {
    spdlog::error("Failed to load {}: {}", path.toStdString(), failure.toStdString());
    statusBar()->showMessage(QString("Failed to load %1: %2").arg(QFileInfo(path).fileName(), failure));
    skipFailed(path, direction);
    return;
  }

  spdlog::info("Loaded {} in {} ms", path.toStdString(), m_LoadTime.elapsed());
  m_CurrentFile = path;
  m_CopyAction->setEnabled(true);
  setWindowTitle(QFileInfo(path).fileName() + " - shtetl");
  const vsg::dvec3 size = m_Viewport->modelBounds().max - m_Viewport->modelBounds().min;
  statusBar()->showMessage(QString("%1  |  %2 x %3 x %4  |  %5")
                               .arg(path)
                               .arg(size.x, 0, 'f', 2)
                               .arg(size.y, 0, 'f', 2)
                               .arg(size.z, 0, 'f', 2)
                               .arg(QLocale().formattedDataSize(QFileInfo(path).size())));
  addRecent(path);
  const QFileInfo info(path);
  m_LoadedModified = info.lastModified();
  m_LoadedSize = info.size();
  watchCurrentFile();
}

void MainWindow::cancelLoad()
{
  if (!m_Load.running) return;
  m_Load.cancel->store(true);
  m_Load.running = false;
  m_Load.id++; // its result is ignored
  m_ProgressTimer.stop();
  m_Progress->setVisible(false);
  statusBar()->showMessage("Loading cancelled", 3000);
}

void MainWindow::updateLoadProgress()
{
  if (!m_Load.running) return;
  // Only a load that takes a while gets a bar; most are over before that.
  if (!m_Progress->isVisible())
  {
    if (m_LoadTime.elapsed() < 300) return;
    m_Progress->setVisible(true);
  }
  const int percent = m_Load.percent->load();
  if (percent < 0)
    m_Progress->setRange(0, 0); // not known: a busy bar
  else
  {
    m_Progress->setRange(0, 100);
    m_Progress->setValue(percent);
  }
}

// A file that cannot be loaded is dropped from the list when stepping through
// the models (next/previous), and the step goes on past it.
void MainWindow::skipFailed(const QString& path, int direction)
{
  if (direction == 0) return;
  const QString next = m_Browser->adjacentFile(direction, path);
  m_Browser->removeFiles({path});
  if (next.isEmpty() || next == path) return;
  if (next == m_CurrentFile)
  {
    m_Browser->selectFile(next, false); // back on the model shown
    return;
  }
  m_StepDirection = direction;
  m_Browser->selectFile(next); // loads it
  m_StepDirection = 0;
}

void MainWindow::watchCurrentFile()
{
  const QStringList watched = m_Watcher.files() + m_Watcher.directories();
  if (!watched.isEmpty()) m_Watcher.removePaths(watched);
  if (!m_AutoReload || m_CurrentFile.isEmpty()) return;
  m_Watcher.addPath(m_CurrentFile);
  m_Watcher.addPath(QFileInfo(m_CurrentFile).absolutePath());
}

void MainWindow::autoReload()
{
  if (!m_AutoReload || m_CurrentFile.isEmpty()) return;
  const QFileInfo info(m_CurrentFile);
  // A replaced file drops off the watch list; it is watched again either way.
  if (!info.exists())
  {
    watchCurrentFile();
    return;
  }
  if (info.lastModified() == m_LoadedModified && info.size() == m_LoadedSize)
  {
    watchCurrentFile();
    return;
  }
  // A half-written file fails to load and keeps the current model; the write
  // finishing changes the file again, and so triggers the next attempt.
  m_LoadedModified = info.lastModified();
  m_LoadedSize = info.size();
  loadModel(m_CurrentFile);
  watchCurrentFile();
}

// The file goes on the clipboard as a file URL (text/uri-list), which is how
// Windows Explorer (CF_HDROP, made by Qt from the URL) and the Linux file
// managers copy files, plus the GNOME/KDE extras and the path as text.
void MainWindow::copyToClipboard()
{
  if (m_CurrentFile.isEmpty()) return;
  const QUrl url = QUrl::fromLocalFile(m_CurrentFile);

  auto* mime = new QMimeData;
  mime->setUrls({url});
  mime->setText(QDir::toNativeSeparators(m_CurrentFile));
#ifndef Q_OS_WIN
  mime->setData("x-special/gnome-copied-files", ("copy\n" + url.toEncoded()));
#endif
  QApplication::clipboard()->setMimeData(mime);
  statusBar()->showMessage("Copied " + QFileInfo(m_CurrentFile).fileName() + " to the clipboard", 3000);
}

void MainWindow::goAdjacent(int delta)
{
  // From the selection, which runs ahead of the model shown while one loads.
  const QString next = m_Browser->adjacentFile(delta);
  if (next.isEmpty()) return;
  m_StepDirection = delta;
  m_Browser->selectFile(next); // loads it
  m_StepDirection = 0;
}

void MainWindow::reload()
{
  m_Browser->refresh(); // loads the selected model again
  if (!m_CurrentFile.isEmpty() && m_Browser->currentFile() != m_CurrentFile) loadModel(m_CurrentFile);
}

void MainWindow::addRecent(const QString& path)
{
  QSettings settings;
  QStringList recent = settings.value("recentFiles").toStringList();
  recent.removeAll(path);
  recent.prepend(path);
  while (recent.size() > MaxRecentFiles) recent.removeLast();
  settings.setValue("recentFiles", recent);
  updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
  m_RecentMenu->clear();
  const QStringList recent = QSettings().value("recentFiles").toStringList();
  for (const QString& file : recent) m_RecentMenu->addAction(file, this, [this, file]() { openPath(file); });
  m_RecentMenu->setEnabled(!recent.isEmpty());
}

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
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

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

  // The mouse side buttons are caught for the whole application, as the 3D
  // view is a native window that vsgQt reads its own events from.
  qApp->installEventFilter(this);

  createMenus();
  statusBar()->showMessage("Open a model or a directory");

  const QSettings settings;
  showDirectory(settings.value("lastDirectory", QDir::homePath()).toString());
}

MainWindow::~MainWindow()
{
  qApp->removeEventFilter(this);
  // Stops the thumbnail thread, which renders on the viewport's device,
  // before the viewport (and the device) is destroyed.
  delete m_Service;
  m_Service = nullptr;
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
  auto* menuBarAction = viewMenu->addAction("Menu Bar", this, [this](bool show) { menuBar()->setVisible(show); });
  menuBarAction->setCheckable(true);
  menuBarAction->setChecked(true);
  menuBarAction->setShortcut(QKeySequence("Ctrl+M"));
  addAction(menuBarAction);
  auto* dockAction = m_BrowserDock->toggleViewAction();
  dockAction->setText("Thumbnails");
  dockAction->setShortcut(QKeySequence("F9"));
  viewMenu->addAction(dockAction);
  viewMenu->addSeparator();
  auto* fit = viewMenu->addAction("Fit to Window", m_Viewport, [this]() { m_Viewport->fitAll(); });
  fit->setShortcuts({QKeySequence("Ctrl+0"), QKeySequence("F")});
  auto* ortho = viewMenu->addAction("Orthographic\tNumpad 5");
  ortho->setCheckable(true);
  connect(ortho, &QAction::toggled, m_Viewport, &Viewport3D::setOrthographic);
  connect(m_Viewport, &Viewport3D::projectionChanged, ortho, &QAction::setChecked);
  viewMenu->addSeparator();

  // The 3D interaction flavor: the same presets as XjetStudio and RobotMotion,
  // persisted under the same key.
  QSettings settings;
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
  next->setShortcuts({QKeySequence("Shift+Down"), QKeySequence("Space"), QKeySequence::Forward});
  auto* previous = viewMenu->addAction("Previous Model", this, [this]() { goAdjacent(-1); });
  previous->setShortcuts({QKeySequence("Shift+Up"), QKeySequence("Backspace"), QKeySequence::Back});

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
  logo->setPixmap(QPixmap(":/icons/shtetl.png").scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
    m_Browser->selectFile(info.absoluteFilePath()); // loads it
    if (m_CurrentFile != info.absoluteFilePath()) loadModel(info.absoluteFilePath()); // not a listed type
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
  try
  {
    spdlog::info("Loading {}", path.toStdString());
    m_Viewport->loadModel(path);
  }
  catch (const std::exception& e)
  {
    spdlog::error("Failed to load {}: {}", path.toStdString(), e.what());
    statusBar()->showMessage(QString("Failed to load %1: %2").arg(QFileInfo(path).fileName(), e.what()));
    return;
  }
  spdlog::info("Loaded {}", path.toStdString());
  m_CurrentFile = path;
  m_CopyAction->setEnabled(true);
  setWindowTitle(QFileInfo(path).fileName() + " - shtetl");
  statusBar()->showMessage(path);
  addRecent(path);
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
  const QString next = m_Browser->adjacentFile(delta);
  if (!next.isEmpty()) m_Browser->selectFile(next);
}

void MainWindow::reload()
{
  m_Browser->refresh();
  if (!m_CurrentFile.isEmpty()) loadModel(m_CurrentFile);
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

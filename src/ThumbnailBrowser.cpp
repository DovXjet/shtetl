#include "ThumbnailBrowser.h"

#include "ModelFormats.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QKeyEvent>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <cmath>
#include <QScrollBar>
#include <QWheelEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
  constexpr int PathRole = Qt::UserRole + 1;
  constexpr int IsDirRole = Qt::UserRole + 2;
  constexpr int ThumbnailPixels = 256; // size of the stored thumbnails

  // Dragging a model out of the browser offers it as a file URL, so it can be
  // dropped into other programs (Explorer, XjetStudio) like a file from Explorer.
  class FileDragModel : public QStandardItemModel
  {
    public:
      using QStandardItemModel::QStandardItemModel;

      Qt::ItemFlags flags(const QModelIndex& index) const override
      {
        Qt::ItemFlags f = QStandardItemModel::flags(index);
        if (index.isValid() && index.data(IsDirRole).toBool()) f &= ~Qt::ItemIsDragEnabled;
        return f;
      }

      QMimeData* mimeData(const QModelIndexList& indexes) const override
      {
        QList<QUrl> urls;
        for (const QModelIndex& index : indexes)
          if (!index.data(IsDirRole).toBool()) urls << QUrl::fromLocalFile(index.data(PathRole).toString());
        if (urls.isEmpty()) return nullptr;
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        return mime;
      }
  };
} // namespace

ThumbnailBrowser::ThumbnailBrowser(ThumbnailService* service, QWidget* parent) : QWidget(parent), m_Service(service)
{
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(2, 2, 2, 2);

  auto* bar = new QHBoxLayout;
  m_UpButton = new QToolButton(this);
  m_UpButton->setText("↑");
  m_UpButton->setToolTip("Parent directory");
  m_PathEdit = new QLineEdit(this);
  m_SizeSlider = new QSlider(Qt::Horizontal, this);
  m_SizeSlider->setRange(64, ThumbnailPixels);
  m_SizeSlider->setFixedWidth(90);
  m_SizeSlider->setToolTip("Thumbnail size");
  bar->addWidget(m_UpButton);
  bar->addWidget(m_PathEdit, 1);
  bar->addWidget(m_SizeSlider);
  layout->addLayout(bar);

  m_Model = new FileDragModel(this);
  m_View = new QListView(this);
  m_View->setModel(m_Model);
  m_View->setViewMode(QListView::IconMode);
  m_View->setResizeMode(QListView::Adjust);
  m_View->setMovement(QListView::Static);
  m_View->setDragEnabled(true);
  m_View->setDragDropMode(QAbstractItemView::DragOnly);
  m_View->setDefaultDropAction(Qt::CopyAction);
  m_View->setUniformItemSizes(true);
  m_View->setWordWrap(true);
  m_View->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_View->setSelectionMode(QAbstractItemView::ExtendedSelection);
  layout->addWidget(m_View, 1);

  // Smooth scrolling: per-pixel positions; wheel notches move a spring's
  // target, and a critically damped spring follows it, so the motion
  // accelerates and decelerates instead of jumping.
  m_View->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_ScrollTimer = new QTimer(this);
  m_ScrollTimer->setInterval(8);
  connect(m_ScrollTimer, &QTimer::timeout, this, [this]() {
    constexpr double omega = 14.0; // spring stiffness, 1/s
    const double dt = m_ScrollTimer->interval() / 1000.0;
    // Semi-implicit Euler for x'' = -2*omega*x' - omega^2*(x - target)
    m_ScrollVel += (-2 * omega * m_ScrollVel - omega * omega * (m_ScrollPos - m_ScrollTarget)) * dt;
    m_ScrollPos += m_ScrollVel * dt;
    QScrollBar* bar = m_View->verticalScrollBar();
    bar->setValue(qRound(m_ScrollPos));
    if (qAbs(m_ScrollPos - m_ScrollTarget) < 0.5 && qAbs(m_ScrollVel) < 5)
    {
      bar->setValue(qRound(m_ScrollTarget));
      m_ScrollTimer->stop();
    }
  });
  m_View->viewport()->installEventFilter(this);
  m_View->installEventFilter(this);

  m_View->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_View, &QWidget::customContextMenuRequested, this, &ThumbnailBrowser::onContextMenu);

  connect(m_UpButton, &QToolButton::clicked, this, [this]() {
    QDir dir(m_Directory);
    if (dir.cdUp()) setDirectory(dir.absolutePath());
  });
  connect(m_PathEdit, &QLineEdit::returnPressed, this, [this]() {
    if (QDir(m_PathEdit->text()).exists()) setDirectory(m_PathEdit->text());
    else m_PathEdit->setText(m_Directory);
  });
  connect(m_SizeSlider, &QSlider::valueChanged, this, &ThumbnailBrowser::setIconSize);
  connect(m_View, &QListView::activated, this, &ThumbnailBrowser::onActivated);
  connect(m_View->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex& current) { onCurrentChanged(current); });
  connect(m_Service, &ThumbnailService::thumbnailReady, this, &ThumbnailBrowser::onThumbnailReady);

  setIconSize(QSettings().value("Browser/ThumbnailSize", 128).toInt());
  m_SizeSlider->setValue(m_View->iconSize().width());
}

ThumbnailBrowser::~ThumbnailBrowser()
{
  if (m_Cancel) m_Cancel->store(true);
}

bool ThumbnailBrowser::eventFilter(QObject* watched, QEvent* event)
{
  // The list owns the keys it types and navigates by: the window's single-key
  // shortcuts (P, A, Left, Space, ...) must not take them (type-ahead search).
  if (event->type() == QEvent::ShortcutOverride && watched == m_View)
  {
    auto* key = static_cast<QKeyEvent*>(event);
    const bool command = key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    bool claim = !command && !key->text().isEmpty() && key->text().at(0).isPrint();
    if (!command)
      claim = claim || key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up
              || key->key() == Qt::Key_Down || key->key() == Qt::Key_Home || key->key() == Qt::Key_End;
    if (claim)
    {
      event->accept();
      return true;
    }
  }
  if (watched == m_View->viewport() && event->type() == QEvent::Wheel)
  {
    auto* wheel = static_cast<QWheelEvent*>(event);
    // Handled even when the event carries a pixelDelta: many mice report one (libinput),
    // and Qt's default would then scroll by hundreds of pixels per notch.
    if (wheel->angleDelta().y() != 0 && !(wheel->modifiers() & Qt::ControlModifier))
    {
      QScrollBar* bar = m_View->verticalScrollBar();
      const double row = m_View->gridSize().height(); // one wheel notch (120) == one thumbnail row
      if (!m_ScrollTimer->isActive())
      {
        m_ScrollPos = m_ScrollTarget = bar->value();
        m_ScrollVel = 0;
      }
      // Fractional deltas from hi-res wheels accumulate, so no rounding here.
      // A single event never moves more than one row, whatever delta the device reports.
      const double notches = qBound(-1.0, wheel->angleDelta().y() / 120.0, 1.0);
      m_ScrollTarget = qBound<double>(bar->minimum(), m_ScrollTarget - notches * row, bar->maximum());
      m_ScrollTimer->start();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void ThumbnailBrowser::focusList() { m_View->setFocus(Qt::OtherFocusReason); }

void ThumbnailBrowser::typeAhead(const QString& text)
{
  m_View->setFocus(Qt::OtherFocusReason);
  m_View->keyboardSearch(text);
}

void ThumbnailBrowser::setIconSize(int size)
{
  m_View->setIconSize(QSize(size, size));
  m_View->setGridSize(QSize(size + 24, size + 48));
  QSettings().setValue("Browser/ThumbnailSize", size);
}

// A glyph on a transparent background, in the colors of XjetStudio's
// thumbnail dialog: grey while the thumbnail is being made, orange when it failed.
QIcon ThumbnailBrowser::placeholderIcon(const QString& text) const
{
  const QColor color = text == "?" ? QColor(220, 120, 40) : QColor(160, 160, 160);
  QPixmap pixmap(ThumbnailPixels, ThumbnailPixels);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setPen(color);
  QFont font = painter.font();
  font.setPixelSize(ThumbnailPixels / 2);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(pixmap.rect(), Qt::AlignCenter, text == "?" ? "!" : text);
  return QIcon(pixmap);
}

// A folder in XjetStudio's colors: pastel yellow with a dark outline.
QIcon ThumbnailBrowser::folderIcon() const
{
  QPixmap pixmap(ThumbnailPixels, ThumbnailPixels);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(QColor(80, 80, 80), ThumbnailPixels / 24.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(QColor(240, 220, 130));
  const double w = ThumbnailPixels, h = ThumbnailPixels;
  QPainterPath path;
  path.moveTo(w * 0.10, h * 0.25);
  path.lineTo(w * 0.40, h * 0.25);
  path.lineTo(w * 0.48, h * 0.34);
  path.lineTo(w * 0.90, h * 0.34);
  path.lineTo(w * 0.90, h * 0.78);
  path.lineTo(w * 0.10, h * 0.78);
  path.closeSubpath();
  painter.drawPath(path);
  return QIcon(pixmap);
}

void ThumbnailBrowser::setDirectory(const QString& path)
{
  const QDir dir(path);
  if (!dir.exists()) return;
  m_Directory = dir.absolutePath();
  m_PathEdit->setText(m_Directory);
  m_UpButton->setEnabled(!dir.isRoot());

  // Thumbnails still queued for the previous directory are not wanted.
  if (m_Cancel) m_Cancel->store(true);
  m_Cancel = std::make_shared<std::atomic<bool>>(false);

  m_Model->clear();
  m_Items.clear();

  const QIcon folder = folderIcon();
  for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase))
  {
    auto* item = new QStandardItem(folder, info.fileName());
    item->setData(info.absoluteFilePath(), PathRole);
    item->setData(true, IsDirRole);
    m_Model->appendRow(item);
  }

  const QIcon pending = placeholderIcon("…");
  for (const QFileInfo& info : dir.entryInfoList(modelNameFilters(), QDir::Files, QDir::Name | QDir::IgnoreCase))
  {
    auto* item = new QStandardItem(pending, info.fileName());
    item->setData(info.absoluteFilePath(), PathRole);
    item->setData(false, IsDirRole);
    item->setToolTip(info.absoluteFilePath());
    m_Model->appendRow(item);
    m_Items.insert(info.absoluteFilePath(), item);
    m_Service->request(info.absoluteFilePath(), m_Cancel);
  }
  emit directoryChanged(m_Directory);
}

void ThumbnailBrowser::refresh()
{
  const QString current = currentFile();
  setDirectory(m_Directory);
  if (!current.isEmpty()) selectFile(current);
}

QString ThumbnailBrowser::currentFile() const
{
  const QModelIndex index = m_View->currentIndex();
  if (!index.isValid() || index.data(IsDirRole).toBool()) return {};
  return index.data(PathRole).toString();
}

bool ThumbnailBrowser::selectFile(const QString& path, bool load)
{
  auto it = m_Items.find(QFileInfo(path).absoluteFilePath());
  if (it == m_Items.end()) return false;
  QItemSelectionModel* selection = m_View->selectionModel();
  const bool wasBlocked = selection->blockSignals(!load);
  m_View->setCurrentIndex(it.value()->index());
  selection->blockSignals(wasBlocked);
  m_View->scrollTo(it.value()->index());
  return true;
}

QString ThumbnailBrowser::adjacentFile(int delta, const QString& from) const
{
  QStringList files;
  for (int row = 0; row < m_Model->rowCount(); ++row)
  {
    const QModelIndex index = m_Model->index(row, 0);
    if (!index.data(IsDirRole).toBool()) files << index.data(PathRole).toString();
  }
  if (files.isEmpty()) return {};
  const int at = files.indexOf(from);
  const int next = at < 0 ? (delta > 0 ? 0 : files.size() - 1) : (at + delta % files.size() + files.size()) % files.size();
  return files[next];
}

void ThumbnailBrowser::removeFiles(const QStringList& paths)
{
  const QSignalBlocker blocker(m_View->selectionModel());
  for (const QString& path : paths)
  {
    auto it = m_Items.find(QFileInfo(path).absoluteFilePath());
    if (it == m_Items.end()) continue;
    m_Model->removeRow(it.value()->row());
    m_Items.erase(it);
  }
}

QStringList ThumbnailBrowser::selectedFiles() const
{
  QStringList files;
  for (const QModelIndex& index : m_View->selectionModel()->selectedIndexes())
    if (!index.data(IsDirRole).toBool()) files << index.data(PathRole).toString();
  files.sort(Qt::CaseInsensitive);
  return files;
}

void ThumbnailBrowser::copyFileToClipboard(const QString& path)
{
  copyFilesToClipboard({path});
}

void ThumbnailBrowser::copyFilesToClipboard(const QStringList& paths)
{
  if (paths.isEmpty()) return;
  QList<QUrl> urls;
  QStringList nativePaths;
  QByteArray gnome = "copy";
  for (const QString& path : paths)
  {
    const QUrl url = QUrl::fromLocalFile(path);
    urls << url;
    nativePaths << QDir::toNativeSeparators(path);
    gnome += "\n" + url.toEncoded();
  }
  auto* mime = new QMimeData;
  mime->setUrls(urls);
  mime->setText(nativePaths.join('\n'));
#ifndef Q_OS_WIN
  mime->setData("x-special/gnome-copied-files", gnome);
#endif
  QApplication::clipboard()->setMimeData(mime);
}

void ThumbnailBrowser::onContextMenu(const QPoint& pos)
{
  const QModelIndex index = m_View->indexAt(pos);
  if (!index.isValid() || index.data(IsDirRole).toBool()) return;
  // Acts on the whole selection when the clicked file is part of it.
  QStringList paths = selectedFiles();
  const QString path = index.data(PathRole).toString();
  if (!paths.contains(path)) paths = {path};
  const bool many = paths.size() > 1;

  QMenu menu(this);
  QAction* copy = menu.addAction(many ? QString("Copy %1 files").arg(paths.size()) : QString("Copy"));
  QAction* refresh = menu.addAction(many ? QString("Refresh %1 thumbnails").arg(paths.size()) : QString("Refresh thumbnail"));
  const QAction* chosen = menu.exec(m_View->viewport()->mapToGlobal(pos));
  if (chosen == copy) copyFilesToClipboard(paths);
  else if (chosen == refresh)
    for (const QString& p : paths) refreshThumbnail(p);
}

void ThumbnailBrowser::refreshThumbnail(const QString& path)
{
  auto it = m_Items.find(path);
  if (it == m_Items.end()) return;
  it.value()->setIcon(placeholderIcon("…"));
  m_Service->request(path, m_Cancel, true);
}

void ThumbnailBrowser::onActivated(const QModelIndex& index)
{
  const QString path = index.data(PathRole).toString();
  if (path.isEmpty()) return;
  if (index.data(IsDirRole).toBool())
    setDirectory(path);
  else
    emit fileActivated(path);
}

void ThumbnailBrowser::onCurrentChanged(const QModelIndex& current)
{
  if (current.isValid() && !current.data(IsDirRole).toBool()) emit fileSelected(current.data(PathRole).toString());
}

void ThumbnailBrowser::onThumbnailReady(const QString& path, const QImage& image)
{
  auto it = m_Items.find(path);
  if (it == m_Items.end()) return; // another directory's, or another browser's
  // No thumbnail means the file could not be loaded: it is not a model we support.
  if (image.isNull())
    removeFiles({path});
  else
    it.value()->setIcon(QIcon(QPixmap::fromImage(image)));
}

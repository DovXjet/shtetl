#include "ThumbnailBrowser.h"

#include "ModelFormats.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
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
  m_View->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_View, 1);

  // Smooth scrolling: per-pixel positions, and a wheel notch moves a fraction
  // of a thumbnail row, eased out, so the eye can follow the motion.
  m_View->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_ScrollAnim = new QPropertyAnimation(m_View->verticalScrollBar(), "value", this);
  m_ScrollAnim->setEasingCurve(QEasingCurve::OutCubic);
  m_ScrollAnim->setDuration(220);
  m_View->viewport()->installEventFilter(this);

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
  if (watched == m_View->viewport() && event->type() == QEvent::Wheel)
  {
    auto* wheel = static_cast<QWheelEvent*>(event);
    // Touchpads already deliver smooth pixel deltas; leave those alone.
    if (wheel->pixelDelta().isNull() && wheel->angleDelta().y() != 0 && !(wheel->modifiers() & Qt::ControlModifier))
    {
      QScrollBar* bar = m_View->verticalScrollBar();
      const int step = qMax(16, m_View->gridSize().height() / 3); // pixels per notch
      const int from = m_ScrollAnim->state() == QAbstractAnimation::Running ? m_ScrollTarget : bar->value();
      m_ScrollTarget = qBound(bar->minimum(), from - wheel->angleDelta().y() * step / 120, bar->maximum());
      m_ScrollAnim->stop();
      m_ScrollAnim->setStartValue(bar->value());
      m_ScrollAnim->setEndValue(m_ScrollTarget);
      m_ScrollAnim->start();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
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

#include "ThumbnailService.h"

#include "CompactMesh.h"
#include "QtPathUtils.h"

#include <vsg/io/read.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstring>
#include <iostream>

namespace
{
  constexpr int ThumbnailSize = 256;

  // The PNG that caches the thumbnail of a model: in ".thumbnails" next to it,
  // or, when that can't be made, in the user's cache keyed by the directory.
  QString thumbnailPath(const QString& modelPath)
  {
    const QFileInfo info(modelPath);
    QDir dir = info.dir();
    QString cacheDir = dir.filePath(".thumbnails");
    if (!QDir(cacheDir).exists() && !dir.mkpath(".thumbnails"))
    {
      const QByteArray hash = QCryptographicHash::hash(dir.absolutePath().toUtf8(), QCryptographicHash::Md5).toHex().left(16);
      cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbnails/" + QString::fromLatin1(hash);
      QDir().mkpath(cacheDir);
    }
    // The whole file name, so that part.stl and part.obj don't share one.
    return cacheDir + "/" + info.fileName() + ".png";
  }
} // namespace

void ThumbnailWorker::initialize(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Options> options)
{
  m_Renderer.setDevice(device);
  m_Renderer.setExtent({ThumbnailSize, ThumbnailSize});
  m_Options = options;
  m_Initialized = true;
}

void ThumbnailWorker::makeThumbnail(const QString& path, CancelFlag cancel)
{
  if (cancel && cancel->load()) return;
  if (!m_Initialized)
  {
    emit resultReady(path, QImage());
    return;
  }

  const QString cachePath = thumbnailPath(path);
  QImage image;

  // A cached thumbnail, unless the model changed since: ours, or else one
  // that XjetStudio made (".thumbnails/part.png" for part.stl) until it
  // follows our naming. Those are only read, never written.
  const QFileInfo modelInfo(path);
  auto loadIfFresh = [&](const QString& file) {
    const QFileInfo info(file);
    if (info.exists() && info.lastModified() >= modelInfo.lastModified()) image.load(file);
  };
  loadIfFresh(cachePath);
  if (image.isNull()) loadIfFresh(QFileInfo(cachePath).path() + "/" + modelInfo.completeBaseName() + ".png");

  if (image.isNull())
  {
    try
    {
      {
        const LoadedModel model = loadModel(path, m_Options, cancel.get());
        if (cancel && cancel->load()) return;
        if (auto data = m_Renderer.render(model.node, model.bounds))
        {
          // The renderer's RGBA8 rows, copied: the data is mapped GPU memory.
          QImage view(static_cast<const uchar*>(data->dataPointer()), int(data->width()), int(data->height()), QImage::Format_RGBA8888);
          image = view.copy();
          image.save(cachePath, "PNG");
        }
      }
    }
    catch (const std::exception& e)
    {
      std::cerr << "thumbnail of " << path.toStdString() << ": " << e.what() << std::endl;
    }
  }
  emit resultReady(path, image);
}

ThumbnailService::ThumbnailService(QObject* parent) : QObject(parent), m_Worker(new ThumbnailWorker)
{
  qRegisterMetaType<vsg::ref_ptr<vsg::Device>>("vsg::ref_ptr<vsg::Device>");
  qRegisterMetaType<vsg::ref_ptr<vsg::Options>>("vsg::ref_ptr<vsg::Options>");
  qRegisterMetaType<CancelFlag>("CancelFlag");

  m_Worker->moveToThread(&m_Thread);
  connect(&m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
  connect(this, &ThumbnailService::initializeWorker, m_Worker, &ThumbnailWorker::initialize);
  connect(this, &ThumbnailService::operate, m_Worker, &ThumbnailWorker::makeThumbnail);
  connect(m_Worker, &ThumbnailWorker::resultReady, this, &ThumbnailService::thumbnailReady);
  m_Thread.start();
}

ThumbnailService::~ThumbnailService()
{
  m_Thread.quit();
  m_Thread.wait();
}

void ThumbnailService::initialize(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Options> options)
{
  emit initializeWorker(device, options);
}

void ThumbnailService::request(const QString& path, CancelFlag cancel)
{
  emit operate(path, cancel);
}

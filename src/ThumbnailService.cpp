#include "ThumbnailService.h"

#include "CompactMesh.h"
#include "Logging.h"
#include "ModelFormats.h"
#include "QtPathUtils.h"

#include <vsg/io/read.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace
{
  constexpr int ThumbnailSize = 256;

  // The colors of the models of XjetStudio's thumbnails (its ThumbnailManager):
  // STL pink, 3MF green. Other formats bring their own materials. The values
  // are sRGB, as one picks them; the shaders take linear light.
  vsg::vec4 thumbnailColor(const QString& path)
  {
    auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    auto color = [&](float r, float g, float b) { return vsg::vec4(linear(r), linear(g), linear(b), 1.0f); };
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "stl") return color(1.0f, 0.8f, 0.8f);
    if (ext == "3mf") return color(0.8f, 1.0f, 0.8f);
    return color(0.93f, 0.93f, 0.93f);
  }

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

void ThumbnailWorker::makeThumbnail(const QString& path, CancelFlag cancel, bool force)
{
  if (cancel && cancel->load()) return;
  if (!m_Initialized)
  {
    emit resultReady(path, QImage());
    return;
  }

  using Clock = std::chrono::steady_clock;
  auto msSince = [](Clock::time_point t) { return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count()); };
  const auto start = Clock::now();
  const QString cachePath = thumbnailPath(path);
  QImage image;
  bool fromCache = true;
  int loadMs = 0, renderMs = 0;

  // A cached thumbnail, unless the model changed since: ours, or else one
  // that XjetStudio made (".thumbnails/part.png" for part.stl) until it
  // follows our naming. Those are only read, never written.
  const QFileInfo modelInfo(path);
  auto loadIfFresh = [&](const QString& file) {
    const QFileInfo info(file);
    if (info.exists() && info.lastModified() >= modelInfo.lastModified()) image.load(file);
  };
  if (!force) loadIfFresh(cachePath);
  if (!force && image.isNull()) loadIfFresh(QFileInfo(cachePath).path() + "/" + modelInfo.completeBaseName() + ".png");

  const int cacheMs = msSince(start);
  if (image.isNull())
  {
    fromCache = false;
    try
    {
      {
        const auto loadStart = Clock::now();
        const LoadedModel model = loadModel(path, m_Options, cancel.get(), thumbnailColor(path));
        loadMs = msSince(loadStart);
        if (cancel && cancel->load()) return;
        // The PBR materials (glTF) are lit by the environment, as in the viewer
        // with the PBR shader: a metallic one has nothing else to show.
        const bool environmentLit = lightWithEnvironment(*model.node);
        const auto renderStart = Clock::now();
        auto data = m_Renderer.render(model.node, model.bounds, environmentLit, &model.compact);
        renderMs = msSince(renderStart);
        if (data)
        {
          // The renderer's RGBA8 rows, copied: the data is mapped GPU memory.
          image = thumbnailImage(*data, QSize(ThumbnailSize, ThumbnailSize));
          image.save(cachePath, "PNG");
        }
      }
    }
    catch (const std::exception& e)
    {
      spdlog::error("Thumbnail of {} failed: {}", path.toStdString(), e.what());
    }
    catch (const vsg::Exception& e) // not a std::exception; would end the program
    {
      spdlog::error("Thumbnail of {} failed: {} (VkResult {})", path.toStdString(), e.message, e.result);
    }
  }
  const int totalMs = msSince(start);
  // Everything here runs on the worker thread, so a slow thumbnail must not
  // be felt in the UI; the log says where the time went if it is.
  if (totalMs >= 500)
    spdlog::warn("Thumbnail of {} took {} ms (cache lookup {}, load {}, render {}, {})", path.toStdString(), totalMs, cacheMs, loadMs, renderMs,
                 fromCache ? "from cache" : "made anew");
  else
    spdlog::debug("Thumbnail of {}: {} ms ({})", path.toStdString(), totalMs, fromCache ? "from cache" : "made anew");
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

void ThumbnailService::request(const QString& path, CancelFlag cancel, bool force)
{
  emit operate(path, cancel, force);
}

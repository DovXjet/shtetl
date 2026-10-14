#pragma once

#include "ThumbnailRenderer.h"

#include <QImage>
#include <QObject>
#include <QThread>

#include <atomic>
#include <memory>

using CancelFlag = std::shared_ptr<std::atomic<bool>>;

// Makes the thumbnail of a model file on a worker thread: a PNG cached in a
// ".thumbnails" directory next to the model (or in the user's cache directory
// when that is read-only), remade when the model is newer. The pattern is
// XjetStudio's CAsyncThumbnailProxy/Worker: the device is handed to the worker
// thread by a queued signal, requests are queued, results come back queued.
class ThumbnailWorker : public QObject
{
    Q_OBJECT
  public slots:
    void initialize(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Options> options);
    void makeThumbnail(const QString& path, CancelFlag cancel, bool force);

  signals:
    void resultReady(const QString& path, const QImage& image);

  private:
    ThumbnailRenderer m_Renderer;
    vsg::ref_ptr<vsg::Options> m_Options;
    bool m_Initialized = false;
};

class ThumbnailService : public QObject
{
    Q_OBJECT
  public:
    explicit ThumbnailService(QObject* parent = nullptr);
    ~ThumbnailService() override;

    // Needs the device of the viewport, so only once that exists. Requests
    // made before are queued behind it.
    void initialize(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Options> options);

    // Queues the thumbnail of a file. It is skipped, if it has not started,
    // once *cancel is true. The result - a null image on failure - comes as
    // thumbnailReady() for whoever listens; consumers ignore paths they don't show.
    // With `force` the cached PNG is ignored and the thumbnail is made anew.
    void request(const QString& path, CancelFlag cancel, bool force = false);

  signals:
    void thumbnailReady(const QString& path, const QImage& image);

    // Internal: the queued hops to the worker thread.
    void initializeWorker(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Options> options);
    void operate(const QString& path, CancelFlag cancel, bool force);

  private:
    QThread m_Thread;
    ThumbnailWorker* m_Worker = nullptr;
};

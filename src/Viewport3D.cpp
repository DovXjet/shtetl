#include "Viewport3D.h"

#include "CompactMesh.h"
#include "ModelFormats.h"
#include "QtPathUtils.h"
#include "StudioLights.h"

#include <vsg/all.h>
#include <vsg/app/CloseHandler.h>
#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/WindowAdapter.h>
#include <vsg/io/read.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/nodes/AbsoluteTransform.h>
#include <vsg/utils/ComputeBounds.h>
#include <vsgQt/Viewer.h>

#include "Logging.h"

#include <QCursor>
#include <QGuiApplication>
#include <QGridLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
  // A right-button click (no drag, no modifier keys) anywhere that the orientation
  // cube did not take. It never marks the events handled, so the navigation
  // presets that rotate with the right button see them as before.
  class ContextClickHandler : public vsg::Inherit<vsg::Visitor, ContextClickHandler>
  {
    public:
      std::function<void()> onClick;

      void apply(vsg::ButtonPressEvent& e) override
      {
        if (e.handled || e.button != 3) return;
        m_Pressed = true;
        m_PressPos = vsg::vec2(e.x, e.y);
      }

      void apply(vsg::ButtonReleaseEvent& e) override
      {
        if (e.handled || e.button != 3 || !m_Pressed) return;
        m_Pressed = false;
        if (vsg::length(vsg::vec2(e.x, e.y) - m_PressPos) < 5 && onClick) onClick();
      }

    private:
      bool m_Pressed = false;
      vsg::vec2 m_PressPos = {0, 0};
  };

  // Hiding the view (closing its window) makes Qt destroy the native window
  // behind the QWindow that createWindowContainer() embeds, and with it the
  // vsg::Window that vsgQt::Window::cleanup() releases. Re-showing builds a
  // new one. Two things in that round trip need help:
  //
  //  - cleanup() clears windowAdapter but leaves traits->systemConnection
  //    pointing at the xcb connection the destroyed vsg::Window has since
  //    disconnected. The next initializeWindow() takes that stale pointer for
  //    an externally supplied connection and reuses it instead of opening a
  //    fresh one, segfaulting inside Xcb_Window's constructor. Clearing it
  //    forces a fresh xcb_connect(), as on the very first init.
  //
  //  - the new vsg::Window is not the one the render graph was built
  //    against, so the view has to be rebuilt (OnReinitialized below).
  //
  //  - cleanup() also clears the viewer, which the window needs to request
  //    a frame on an expose, a resize or an input event; it is put back.
  //
  // Note vsgQt::Window::exposeEvent() calls initializeWindow() on *every*
  // expose - its _initialized flag is never actually set - so the hooks fire
  // only when there is really a new window to set up, not on every repaint.
  class CReinitSafeWindow : public vsgQt::Window
  {
    public:
      using vsgQt::Window::Window;

      // Invoked after a reinitialization that actually built a new
      // vsg::Window, except the first. The owning Viewport3D hooks this to
      // rebuild its render graph, which otherwise keeps pointing at the
      // vsg::Window instance this replaces.
      std::function<void()> OnReinitialized;

      // Invoked just before the base class tears the native window down (see
      // event() below), while the render graph still refers to the
      // vsg::Window that is about to go. The owning Viewport3D hooks this
      // to drop those references, so nothing tries to render into the dead
      // window between the close and the next show.
      std::function<void()> OnAboutToCleanup;

      void initializeWindow() override
      {
        // What the base class does when there is nothing to initialize. Doing
        // it here as well keeps the hooks below off the repaint path.
        if (windowAdapter) return;

        // cleanup() also drops the viewer, as if the window were going for
        // good. Without it every expose, resize and input event of the new
        // window leaves the viewer without a request, so that, rendering on
        // demand, it never draws a frame again.
        if (viewer)
          m_Viewer = viewer;
        else
          viewer = m_Viewer;

        traits->systemConnection = {};
        vsgQt::Window::initializeWindow();
        if (!windowAdapter) return;

        if (m_EverInitialized && OnReinitialized) OnReinitialized();
        m_EverInitialized = true;
      }

    protected:
      bool event(QEvent* e) override
      {
        if (e->type() == QEvent::PlatformSurface)
        {
          auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(e);
          if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && OnAboutToCleanup)
            OnAboutToCleanup();
        }
        return vsgQt::Window::event(e);
      }

    private:
      bool m_EverInitialized = false;
      vsg::ref_ptr<vsgQt::Viewer> m_Viewer; // what cleanup() drops, see initializeWindow()
  };

  // The Vulkan device of the viewports that are open. This build of VSG
  // allows one vsg::Device (VSG_MAX_DEVICES == 1), so every viewport, not
  // only a re-shown one, shares it. Weak, so that the device goes with the
  // last viewport and a later one creates a new one.
  vsg::observer_ptr<vsg::Device> g_SharedDevice;

  float SrgbToLinear(float Value)
  {
    return Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
  }

  vsg::vec3 srgbVec(const QColor& c)
  {
    return {static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF())};
  }

  // The occlusion: how far it reaches (as a fraction of the model's radius)
  // and how much it darkens.
  constexpr double AmbientOcclusionRadius = 0.12;
  constexpr float AmbientOcclusionStrength = 0.85f;


} // namespace

Viewport3D::Viewport3D(QWidget* parent) : QWidget(parent)
{
  m_Traits = vsg::WindowTraits::create();
  m_Traits->windowTitle = "shtetl";
  // Multisampled (a device that can't do this many gets the most it can).
  m_Traits->samples = VK_SAMPLE_COUNT_4_BIT;

  auto viewer = COnDemandViewer::create(8);
  // On-demand rendering: camera interaction calls request() itself.
  viewer->continuousUpdate = false;
  m_Viewer = viewer;

  auto* window = new CReinitSafeWindow(viewer, m_Traits, static_cast<QWindow*>(nullptr));
  m_Window = window;
  // A re-shown window is a new vsg::Window; the render graph must follow it.
  window->OnReinitialized = [this]() { rebuildSceneGraph(); };
  window->OnAboutToCleanup = [this]() {
    if (m_Viewer)
    {
      m_Viewer->recordAndSubmitTasks.clear();
      m_Viewer->presentations.clear();
    }
  };
  m_Window->setTitle("shtetl");
  // vsgQt can't tell the keypad's 5 from the main row's, so it is caught here.
  m_Window->installEventFilter(this);

  if (vsg::ref_ptr<vsg::Device> SharedDevice = g_SharedDevice.ref_ptr())
    m_Window->traits->device = SharedDevice;

  // Build the Vulkan surface/device/swapchain while the QWindow is still an
  // unparented, frameless window, *then* embed it with createWindowContainer()
  // (the order matters, as in RobotMotion).
  m_Window->setFlags(m_Window->flags() | Qt::FramelessWindowHint);
  m_Window->initializeWindow();

  auto layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  m_Container = QWidget::createWindowContainer(m_Window, this);
  m_Container->setFocusPolicy(Qt::StrongFocus);
  layout->addWidget(m_Container, 0, 0);
  m_Container->installEventFilter(this);

  m_Options = createModelOptions();

  m_Viewer->addEventHandler(vsg::CloseHandler::create(m_Viewer));

  // The images of the occlusion have the size of the window: after it has
  // been resized (and left alone for a moment) they are made anew.
  m_OcclusionResizeTimer.setSingleShot(true);
  m_OcclusionResizeTimer.setInterval(150);
  connect(&m_OcclusionResizeTimer, &QTimer::timeout, this, [this]() {
    if (m_Occlusion && m_PixelExtent.width > 0 && (m_Occlusion->extent().width != m_PixelExtent.width || m_Occlusion->extent().height != m_PixelExtent.height))
      rebuildSceneGraph();
  });

  // Compile a valid (if empty) scene graph up front: this is what builds the
  // Vulkan device/surface/swapchain, which the first resize event needs.
  buildSceneGraph();

  // Keep this one Vulkan device (VSG_MAX_DEVICES == 1), shared with the
  // thumbnailer and a re-shown window.
  if (m_Window->windowAdapter)
  {
    m_Window->traits->device = m_Window->windowAdapter->getOrCreateDevice();
    g_SharedDevice = m_Window->traits->device;
  }
}

Viewport3D::~Viewport3D()
{
  if (m_Viewer) m_Viewer->deviceWaitIdle();
}

vsg::ref_ptr<vsg::Device> Viewport3D::device() const
{
  return m_Window && m_Window->windowAdapter ? m_Window->windowAdapter->getOrCreateDevice() : vsg::ref_ptr<vsg::Device>();
}

void Viewport3D::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  onContainerResized();
}

void Viewport3D::onContainerResized()
{
  if (!m_Window || !m_Window->windowAdapter) return;
  const QSize size = m_Container->size();
  if (m_Window->geometry().size() != size) m_Window->resize(size);

  // Derive the extent from the widget's size: the native window's own resize
  // confirmation is asynchronous and can still report the previous size.
  double dpr = m_Window->devicePixelRatio();
  uint32_t width = static_cast<uint32_t>(std::round(size.width() * dpr));
  uint32_t height = static_cast<uint32_t>(std::round(size.height() * dpr));
  if (width == 0 || height == 0) return;
  if (m_Camera && m_Camera->viewportState) m_Camera->viewportState->set(0, 0, width, height);
  if (m_Projection) m_Projection->aspectRatio = static_cast<double>(width) / static_cast<double>(height);
  m_PixelExtent = VkExtent2D{width, height};
  if (m_Occlusion)
  {
    m_Occlusion->compositionCamera()->viewportState->set(0, 0, width, height);
    if (m_Occlusion->extent().width != width || m_Occlusion->extent().height != height) m_OcclusionResizeTimer.start();
  }
  if (m_Gradient) m_Gradient->camera()->viewportState->set(0, 0, width, height);
  m_Viewer->request();
}

void Viewport3D::loadModel(const QString& path)
{
  // Loaded on the side, so that a bad file leaves the current model in place.
  setModel(::loadModel(path, m_Options));
}

void Viewport3D::setModel(LoadedModel loaded)
{
  // Keep the view orientation across models; only the framing is refit.
  auto previous = m_Model && m_Camera ? m_Camera->viewMatrix.cast<vsg::LookAt>() : nullptr;
  vsg::dvec3 toEye, up;
  if (previous)
  {
    toEye = previous->eye - previous->center;
    up = previous->up;
  }

  const auto oldModel = m_Model;
  const auto oldBounds = m_ModelBounds;
  const auto oldCompact = m_Compact;
  m_Model = loaded.node;
  m_ModelBounds = loaded.bounds;
  m_Compact = loaded.compact;
  collectMaterials();
  updateCompactMaterial(false);
  try
  {
    buildSceneGraph();
  }
  catch (const vsg::Exception& e)
  {
    // Typically the GPU running out of memory while the model is uploaded.
    // vsg::Exception is not a std::exception, and the viewer was left half
    // rebuilt (no render task), which shows as a view that never responds
    // again - so put the previous model back and say what happened.
    spdlog::error("Building the scene failed: {} (VkResult {})", e.message, e.result);
    m_Model = oldModel;
    m_ModelBounds = oldBounds;
    m_Compact = oldCompact;
    collectMaterials();
    updateCompactMaterial(false);
    loaded = {};
    try
    {
      buildSceneGraph();
    }
    catch (const vsg::Exception& again)
    {
      spdlog::error("Restoring the previous scene failed too: {}", again.message);
    }
    throw std::runtime_error("Out of GPU resources: " + e.message);
  }

  if (previous && vsg::length(toEye) > 0.0)
  {
    if (auto lookAt = m_Camera->viewMatrix.cast<vsg::LookAt>())
    {
      lookAt->eye = lookAt->center + toEye;
      lookAt->up = up;
    }
  }
  fitAll(false);
}

void Viewport3D::clearModel()
{
  m_Model = nullptr;
  m_ModelBounds = vsg::dbox();
  m_Compact = {};
  m_PbrMaterials.clear();
  buildSceneGraph();
}

void Viewport3D::rebuildSceneGraph()
{
  auto previous = m_Camera ? m_Camera->viewMatrix.cast<vsg::LookAt>() : nullptr;
  vsg::dvec3 eye, center, up;
  if (previous)
  {
    eye = previous->eye;
    center = previous->center;
    up = previous->up;
  }

  buildSceneGraph();

  if (previous)
  {
    if (auto restored = m_Camera->viewMatrix.cast<vsg::LookAt>())
    {
      restored->eye = eye;
      restored->center = center;
      restored->up = up;
    }
  }
}

void Viewport3D::buildSceneGraph()
{
  auto root = vsg::Group::create();
  root->addChild(createStudioLights());

  vsg::dvec3 center(0.0, 0.0, 0.0);
  double radius = 1.0;
  if (m_Model)
  {
    root->addChild(m_Model);

    if (m_ModelBounds.valid())
    {
      center = (m_ModelBounds.min + m_ModelBounds.max) * 0.5;
      double computedRadius = vsg::length(m_ModelBounds.max - m_ModelBounds.min) * 0.6;
      if (computedRadius > 0.0) radius = computedRadius;
    }
    m_FitRadius = radius / 1.2; // half the bounds diagonal
  }
  else
    m_FitRadius = 1.0;
  m_FitCenter = center;

  auto lookAt = vsg::LookAt::create(center + vsg::dvec3(radius, -radius, radius), center, vsg::dvec3(0.0, 0.0, 1.0));
  auto extent = m_Window->windowAdapter->extent2D();
  double aspect = extent.height > 0 ? static_cast<double>(extent.width) / static_cast<double>(extent.height) : 1.0;
  m_Projection = CToggleProjection::create(lookAt, 30.0, aspect, radius * 0.01, radius * 10.0);
  m_Projection->orthographic = m_Orthographic;
  m_Camera = vsg::Camera::create(m_Projection, lookAt, vsg::ViewportState::create(extent));

  // Drop the previous trackball and gizmo - they hold the now-stale camera.
  auto& handlers = m_Viewer->getEventHandlers();
  if (m_Trackball) handlers.erase(std::remove(handlers.begin(), handlers.end(), m_Trackball), handlers.end());
  if (m_ContextClick) handlers.erase(std::remove(handlers.begin(), handlers.end(), m_ContextClick), handlers.end());
  if (m_OcclusionUpdater) handlers.erase(std::remove(handlers.begin(), handlers.end(), m_OcclusionUpdater), handlers.end());
  m_Occlusion = nullptr;
  m_OcclusionUpdater = nullptr;
  m_Gradient = nullptr;
  if (m_Gizmo)
  {
    handlers.erase(std::remove(handlers.begin(), handlers.end(), m_Gizmo->GetIntersectionHandler()), handlers.end());
    handlers.erase(std::remove(handlers.begin(), handlers.end(), m_Gizmo->GetContextMenuHandler()), handlers.end());
  }

  m_Trackball = MyTrackball::create(m_Camera);
  m_Trackball->applyNavigationStyle(m_NavigationStyle);
  // Keep pumping frames while a viewpoint animation is interpolating. Queued:
  // this runs inside vsgQt::Viewer::render(), which would drop a direct request().
  m_Trackball->setRedrawRequestCallback([this]() { QMetaObject::invokeMethod(this, [this]() { m_Viewer->request(); }, Qt::QueuedConnection); });
  addKeyViewpoints(center, vsg::length(lookAt->eye - lookAt->center));

  m_Gizmo = CCubeGizmo::create(m_Options, m_Camera, m_Trackball);
  m_Gizmo->SetContextMenuCallback([this]() {
    // Queued: QMenu::exec() would re-enter the event loop from the viewer's event dispatch.
    QMetaObject::invokeMethod(this, [this]() { showGizmoMenu(); }, Qt::QueuedConnection);
  });

  // The cube's context menu first (some presets bind the right button), then
  // the trackball, then the cube's click handler (bare LMB falls through to it).
  m_Viewer->addEventHandler(m_Gizmo->GetContextMenuHandler());
  // Then a plain right-click elsewhere asks for the view's context menu.
  auto contextClick = ContextClickHandler::create();
  m_ContextClick = contextClick;
  contextClick->onClick = [this]() {
    // Queued, like the cube's menu. Ctrl/Shift/Alt + right button navigates.
    if (QGuiApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier)) return;
    const QPoint pos = QCursor::pos();
    QMetaObject::invokeMethod(this, [this, pos]() { emit contextMenuRequested(pos); }, Qt::QueuedConnection);
  };
  m_Viewer->addEventHandler(m_ContextClick);
  m_Viewer->addEventHandler(m_Trackball);
  m_Viewer->addEventHandler(m_Gizmo->GetIntersectionHandler());

  // The frames still in flight draw with the old scene graph and its buffers;
  // let them finish before those are released (a 1 GB model's buffers pile up
  // quickly when models follow each other).
  if (!m_Viewer->recordAndSubmitTasks.empty()) m_Viewer->deviceWaitIdle();
  m_Viewer->recordAndSubmitTasks.clear();
  m_Viewer->presentations.clear();
  auto view3D = vsg::Group::create();
  if (m_AmbientOcclusion && m_Model && m_Compact)
  {
    // The model is drawn into the G-buffer by offscreen passes, and the view
    // of the window is the composition that darkens it by the occlusion.
    auto device = m_Window->windowAdapter->getOrCreateDevice();
    auto gbufferScene = vsg::Group::create();
    gbufferScene->addChild(makeCompactGBufferNode(m_Compact));
    const VkExtent2D occlusionExtent = m_PixelExtent.width > 0 ? m_PixelExtent : extent;
    m_Occlusion = AmbientOcclusion::create(device, m_Camera, gbufferScene, occlusionExtent, m_FitRadius * AmbientOcclusionRadius, srgbVec(m_Background),
                                     m_GradientBackground ? srgbVec(m_BackgroundBottom) : srgbVec(m_Background));
    m_Occlusion->setStrength(AmbientOcclusionStrength);
    m_OcclusionUpdater = AmbientOcclusionUpdater::create();
    m_OcclusionUpdater->occlusion = m_Occlusion;
    m_Viewer->addEventHandler(m_OcclusionUpdater);
    view3D->addChild(m_Occlusion->compositionView());
  }
  else
  {
    if (m_GradientBackground)
    {
      // Before the model; it writes no depth.
      const VkExtent2D gradientExtent = m_PixelExtent.width > 0 ? m_PixelExtent : extent;
      m_Gradient = GradientBackground::create(gradientExtent, m_Window->windowAdapter->framebufferSamples(), srgbVec(m_Background), srgbVec(m_BackgroundBottom));
      view3D->addChild(m_Gradient->view());
    }
    auto view = vsg::View::create(m_Camera);
    view->addChild(root);
    view3D->addChild(view);
  }
  view3D->addChild(m_Gizmo->GetView());

  m_RenderGraph = vsg::RenderGraph::create(m_Window->windowAdapter);
  m_RenderGraph->addChild(view3D);
  m_RenderGraph->setClearValues(VkClearColorValue{{backgroundLinear().r, backgroundLinear().g, backgroundLinear().b, 1.0f}});
  auto commandGraph = vsg::CommandGraph::create(m_Window->windowAdapter);
  if (m_Occlusion)
    for (auto& graph : m_Occlusion->renderGraphs()) commandGraph->addChild(graph);
  commandGraph->addChild(m_RenderGraph);
  m_Viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});
  // A fresh compile manager, as the frames it copies data with were replaced.
  m_Viewer->compileManager = nullptr;
  m_Viewer->compile();
  onContainerResized();
  m_Viewer->request();
}

// View-direction hotkeys, as in XjetStudio: 7/Home = top, 1/End = front, 3/PageDown = right.
void Viewport3D::addKeyViewpoints(const vsg::dvec3& center, double distance)
{
  auto lookAtTop = vsg::LookAt::create(center + vsg::dvec3(0.0, 0.0, distance), center, vsg::dvec3(0.0, 1.0, 0.0));
  auto lookAtFront = vsg::LookAt::create(center + vsg::dvec3(0.0, -distance, 0.0), center, vsg::dvec3(0.0, 0.0, 1.0));
  auto lookAtRight = vsg::LookAt::create(center + vsg::dvec3(distance, 0.0, 0.0), center, vsg::dvec3(0.0, 0.0, 1.0));

  for (auto key : {vsg::KEY_7, vsg::KEY_KP_7, vsg::KEY_KP_Home, vsg::KEY_Home})
    m_Trackball->addKeyViewpoint(key, lookAtTop, 0.5);
  for (auto key : {vsg::KEY_1, vsg::KEY_KP_1, vsg::KEY_KP_End, vsg::KEY_End})
    m_Trackball->addKeyViewpoint(key, lookAtFront, 0.5);
  for (auto key : {vsg::KEY_3, vsg::KEY_KP_3, vsg::KEY_KP_Page_Down, vsg::KEY_Page_Down})
    m_Trackball->addKeyViewpoint(key, lookAtRight, 0.5);
}

void Viewport3D::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  // Keyboard focus at launch so the view hotkeys work without a first click.
  QTimer::singleShot(0, this, [this]() {
    m_Container->setFocus(Qt::OtherFocusReason);
    m_Window->requestActivate();
  });
}

bool Viewport3D::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_Container && event->type() == QEvent::Resize) onContainerResized();

  // QWindowContainer answers a FocusIn by activating the embedded window,
  // which loops when Qt gives the focus back because the top level was activated.
  if (watched == m_Container && event->type() == QEvent::FocusIn
      && static_cast<QFocusEvent*>(event)->reason() == Qt::ActiveWindowFocusReason)
    return true;

  if (watched == m_Window && event->type() == QEvent::KeyPress)
  {
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    // With NumLock off the keypad's 5 arrives as Key_Clear.
    bool isKeypad5 = (keyEvent->modifiers() & Qt::KeypadModifier) && (keyEvent->key() == Qt::Key_5 || keyEvent->key() == Qt::Key_Clear);
    if (isKeypad5)
    {
      if (!keyEvent->isAutoRepeat()) toggleProjection();
      return true;
    }
    if (keyEvent->key() == Qt::Key_F && keyEvent->modifiers() == Qt::NoModifier)
    {
      if (!keyEvent->isAutoRepeat()) fitAll(true);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

// Keeps the current view direction and centers the whole model in view.
void Viewport3D::fitAll(bool animate)
{
  auto lookAt = m_Camera ? m_Camera->viewMatrix.cast<vsg::LookAt>() : nullptr;
  if (!lookAt || !m_Projection) return;

  // The bounding sphere must fit the narrower of the two view angles.
  double halfAngle = std::atan(std::tan(vsg::radians(m_Projection->fieldOfViewY) * 0.5) * std::min(1.0, m_Projection->aspectRatio));
  double distance = m_FitRadius * 1.05 / std::sin(halfAngle);

  vsg::dvec3 toEye = lookAt->eye - lookAt->center;
  if (vsg::length(toEye) < 1e-9) toEye = vsg::dvec3(1.0, -1.0, 1.0);
  auto target = vsg::LookAt::create(m_FitCenter + vsg::normalize(toEye) * distance, m_FitCenter, lookAt->up);

  if (animate && m_Trackball)
    m_Trackball->setViewpoint(target, 0.5);
  else
  {
    lookAt->eye = target->eye;
    lookAt->center = target->center;
  }
  m_Viewer->request();
}

void Viewport3D::setAmbientOcclusion(bool enabled)
{
  if (enabled == m_AmbientOcclusion) return;
  m_AmbientOcclusion = enabled;
  if (m_Camera && m_Window && m_Window->windowAdapter) rebuildSceneGraph();
}

vsg::vec3 Viewport3D::backgroundLinear() const
{
  // The swapchain is sRGB and converts the linear clear color when it is stored.
  return {SrgbToLinear(static_cast<float>(m_Background.redF())), SrgbToLinear(static_cast<float>(m_Background.greenF())),
          SrgbToLinear(static_cast<float>(m_Background.blueF()))};
}

void Viewport3D::applyBackground()
{
  if (!m_Camera || !m_Window || !m_Window->windowAdapter) return;
  const vsg::vec3 top = srgbVec(m_Background);
  const vsg::vec3 bottom = m_GradientBackground ? srgbVec(m_BackgroundBottom) : top;
  const vsg::vec3 linear = backgroundLinear();
  if (m_RenderGraph) m_RenderGraph->setClearValues(VkClearColorValue{{linear.r, linear.g, linear.b, 1.0f}});
  if (m_Occlusion) m_Occlusion->setBackground(top, bottom);
  if (m_Gradient) m_Gradient->setColors(top, bottom);
  m_Viewer->request();
}

void Viewport3D::setBackground(const QColor& color)
{
  if (!color.isValid() || color == m_Background) return;
  m_Background = color;
  applyBackground();
}

void Viewport3D::setBackgroundBottom(const QColor& color)
{
  if (!color.isValid() || color == m_BackgroundBottom) return;
  m_BackgroundBottom = color;
  applyBackground();
}

void Viewport3D::setGradientBackground(bool enabled)
{
  if (enabled == m_GradientBackground) return;
  m_GradientBackground = enabled;
  // The plain view draws the gradient in a view of its own, which comes or goes
  // with the scene graph; the occlusion's composition just takes other colors.
  if (m_Occlusion) applyBackground();
  else if (m_Camera && m_Window && m_Window->windowAdapter) rebuildSceneGraph();
}

namespace
{
  // Finds the PBR material values in a loaded scene (the glTF loader puts them
  // in the descriptor sets of its state groups).
  class FindPbrMaterials : public vsg::Visitor
  {
    public:
      std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> materials;

      void apply(vsg::Object& object) override { object.traverse(*this); }

      void apply(vsg::StateGroup& stateGroup) override
      {
        for (auto& command : stateGroup.stateCommands)
        {
          if (auto bind = command.cast<vsg::BindDescriptorSet>()) scan(bind->descriptorSet);
          else if (auto binds = command.cast<vsg::BindDescriptorSets>())
            for (auto& set : binds->descriptorSets) scan(set);
        }
        stateGroup.traverse(*this);
      }

    private:
      void scan(const vsg::ref_ptr<vsg::DescriptorSet>& set)
      {
        if (!set) return;
        for (auto& descriptor : set->descriptors)
        {
          auto buffer = descriptor.cast<vsg::DescriptorBuffer>();
          if (!buffer) continue;
          for (auto& info : buffer->bufferInfoList)
            if (auto material = info ? info->data.cast<vsg::PbrMaterialValue>() : vsg::ref_ptr<vsg::PbrMaterialValue>())
              if (std::find(materials.begin(), materials.end(), material) == materials.end()) materials.push_back(material);
        }
      }
  };
} // namespace

void Viewport3D::collectMaterials()
{
  m_PbrMaterials.clear();
  if (!m_Model) return;
  FindPbrMaterials finder;
  m_Model->accept(finder);
  for (auto& material : finder.materials)
  {
    // Before the scene graph is compiled: only data that is dynamic can be
    // changed afterwards.
    material->properties.dataVariance = vsg::DYNAMIC_DATA;
    m_PbrMaterials.push_back({material, material->value().metallicFactor, material->value().roughnessFactor});
  }
  if (!m_PbrMaterials.empty()) spdlog::debug("{} PBR material(s) found", m_PbrMaterials.size());
  applyMaterialOverride();
}

void Viewport3D::applyMaterialOverride()
{
  for (auto& state : m_PbrMaterials)
  {
    state.material->value().metallicFactor = m_OverrideMaterial ? m_OverrideMetallic : state.metallic;
    state.material->value().roughnessFactor = m_OverrideMaterial ? m_OverrideRoughness : state.roughness;
    state.material->dirty();
  }
}

void Viewport3D::setMaterialOverride(bool enabled, float metallic, float roughness)
{
  m_OverrideMaterial = enabled;
  m_OverrideMetallic = metallic;
  m_OverrideRoughness = roughness;
  applyMaterialOverride();
  updateCompactMaterial(true);
  if (m_Viewer && !m_PbrMaterials.empty()) m_Viewer->request();
}

void Viewport3D::updateCompactMaterial(bool rebuild)
{
  if (!m_Compact || !m_Model) return;
  auto& material = m_Compact.pbrMaterial->value();
  material.metallicFactor = m_OverrideMetallic;
  material.roughnessFactor = m_OverrideRoughness;
  material.baseColorFactor = m_Compact.diffuse;
  m_Compact.pbrMaterial->dirty();
  if (m_Compact.pbr != m_OverrideMaterial)
  {
    // Another shader: another node (and pipeline).
    m_Compact.pbr = m_OverrideMaterial;
    m_Model = makeCompactNode(m_Compact);
    if (rebuild && m_Camera && m_Window && m_Window->windowAdapter) rebuildSceneGraph();
  }
  else if (m_Viewer)
    m_Viewer->request();
}

void Viewport3D::setModelColor(const vsg::vec4& diffuse)
{
  if (!m_Compact || !m_Model) return;
  m_Compact.diffuse = diffuse;
  if (m_Compact.pbr)
  {
    // The material carries the color: nothing to rebuild.
    updateCompactMaterial(false);
    return;
  }
  m_Model = makeCompactNode(m_Compact);
  if (m_Camera && m_Window && m_Window->windowAdapter) rebuildSceneGraph();
}

void Viewport3D::setOrthographic(bool orthographic)
{
  if (orthographic == m_Orthographic) return;
  m_Orthographic = orthographic;
  if (m_Projection) m_Projection->orthographic = m_Orthographic;
  m_Viewer->request();
  emit projectionChanged(m_Orthographic);
}

void Viewport3D::showGizmoMenu()
{
  QMenu menu(this);
  auto* action = menu.addAction("Orthographic");
  action->setCheckable(true);
  action->setChecked(m_Orthographic);
  connect(action, &QAction::toggled, this, &Viewport3D::setOrthographic);
  menu.exec(QCursor::pos());
}

void Viewport3D::setNavigationStyle(NavigationStyle style)
{
  m_NavigationStyle = style;
  if (m_Trackball) m_Trackball->applyNavigationStyle(style);
}

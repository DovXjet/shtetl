#pragma once

#include "AmbientOcclusion.h"
#include "CompactMesh.h"
#include "CubeGizmo.h"
#include "MyTrackball.h"
#include "NavigationStyles.h"
#include "OnDemandViewer.h"

#include <vsgQt/Window.h>

#include <vsg/app/RenderGraph.h>
#include <vsg/maths/transform.h>

#include <QColor>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cmath>

// Perspective projection that can be switched to an orthographic one with the
// same framing at the look-at center (from RobotMotion).
class CToggleProjection : public vsg::Inherit<vsg::ProjectionMatrix, CToggleProjection>
{
  public:
    CToggleProjection(vsg::ref_ptr<vsg::LookAt> lookAt, double fovY, double aspect, double nearDist, double farDist)
        : fieldOfViewY(fovY), aspectRatio(aspect), nearDistance(nearDist), farDistance(farDist), m_LookAt(lookAt)
    {
    }

    vsg::dmat4 transform() const override
    {
        if (!orthographic) return vsg::perspective(vsg::radians(fieldOfViewY), aspectRatio, nearDistance, farDistance);
        double halfH = vsg::length(m_LookAt->eye - m_LookAt->center) * std::tan(vsg::radians(fieldOfViewY) * 0.5);
        double halfW = halfH * aspectRatio;
        // Negative near plane so zooming in past the model doesn't clip it.
        return vsg::orthographic(-halfW, halfW, -halfH, halfH, -farDistance, farDistance);
    }

    bool orthographic = false;
    double fieldOfViewY, aspectRatio, nearDistance, farDistance;

  private:
    vsg::ref_ptr<vsg::LookAt> m_LookAt;
};

// The 3D view of one model, with the orientation cube in the corner.
class Viewport3D : public QWidget
{
    Q_OBJECT

  public:
    explicit Viewport3D(QWidget* parent = nullptr);
    ~Viewport3D() override;

    // Replaces the model shown. Throws std::runtime_error on failure, in which
    // case the current model stays.
    void loadModel(const QString& path);
    // Shows a model loaded elsewhere (::loadModel(), which may run on another
    // thread); this part has to run on the GUI thread. Throws as loadModel().
    void setModel(LoadedModel loaded);
    void clearModel();
    // Bounds of the model shown, in its own coordinates.
    vsg::dbox modelBounds() const { return m_ModelBounds; }

    // Mouse button mapping and rotation feel (see NavigationStyles.h).
    void setNavigationStyle(NavigationStyle style);
    NavigationStyle navigationStyle() const { return m_NavigationStyle; }

    // Keeps the view direction and frames the whole model ("f" key).
    void fitAll(bool animate = true);

    // Darkens the creases of the model (screen space ambient occlusion, as in
    // XjetStudio). Only the STL and 3MF models, which are drawn by shtetl's own
    // shaders, take part; the others are shown as before.
    void setAmbientOcclusion(bool enabled);
    bool isAmbientOcclusion() const { return m_AmbientOcclusion; }

    // The color behind the model (sRGB). The default is XjetStudio's deep blue.
    void setBackground(const QColor& color);
    QColor background() const { return m_Background; }
    static QColor defaultBackground() { return QColor(0x33, 0x33, 0x66); }

    // The color of the STL and 3MF model shown (linear rgb), at once; later
    // models are loaded with their own color argument.
    void setModelColor(const vsg::vec4& diffuse);

    void toggleProjection() { setOrthographic(!m_Orthographic); }
    void setOrthographic(bool orthographic);
    bool isOrthographic() const { return m_Orthographic; }

    // For the thumbnailer, which renders on the same (one and only) device.
    vsg::ref_ptr<vsg::Device> device() const;
    vsg::ref_ptr<vsg::Options> options() const { return m_Options; }

  signals:
    void projectionChanged(bool orthographic);
    // A plain right-click in the view (not a drag, not on the orientation cube).
    void contextMenuRequested(const QPoint& globalPos);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void onContainerResized();
    void buildSceneGraph();
    // buildSceneGraph(), keeping the viewpoint the user was looking from.
    void rebuildSceneGraph();
    void showGizmoMenu();
    vsg::vec3 backgroundLinear() const;
    void addKeyViewpoints(const vsg::dvec3& center, double distance);

    vsg::ref_ptr<vsg::WindowTraits> m_Traits;
    vsg::ref_ptr<COnDemandViewer> m_Viewer;
    vsgQt::Window* m_Window = nullptr; // owned by Qt
    QWidget* m_Container = nullptr;
    vsg::ref_ptr<vsg::Options> m_Options;
    vsg::ref_ptr<vsg::Camera> m_Camera;
    vsg::ref_ptr<CToggleProjection> m_Projection;
    vsg::ref_ptr<MyTrackball> m_Trackball;
    vsg::ref_ptr<CCubeGizmo> m_Gizmo;
    vsg::ref_ptr<vsg::Visitor> m_ContextClick;
    vsg::ref_ptr<vsg::RenderGraph> m_RenderGraph;
    vsg::ref_ptr<vsg::Node> m_Model;
    vsg::dbox m_ModelBounds;
    CompactSource m_Compact; // of the model, when it is one
    QColor m_Background = defaultBackground();
    bool m_AmbientOcclusion = false;
    vsg::ref_ptr<AmbientOcclusion> m_Occlusion; // while it is in the scene graph
    vsg::ref_ptr<AmbientOcclusionUpdater> m_OcclusionUpdater;
    // The size of the window in pixels, as of the last resize; the occlusion's
    // images are made at that size, and made anew (after a pause) when it changes.
    VkExtent2D m_PixelExtent{0, 0};
    QTimer m_OcclusionResizeTimer;
    vsg::dvec3 m_FitCenter{0.0, 0.0, 0.0};
    double m_FitRadius = 1.0;
    bool m_Orthographic = false;
    NavigationStyle m_NavigationStyle = NavigationStyle::Netfabb;
};

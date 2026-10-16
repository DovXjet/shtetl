#pragma once

#include "AmbientOcclusion.h"
#include "AnimationPlayer.h"
#include "CompactMesh.h"
#include "CubeGizmo.h"
#include "EdgeMesh.h"
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

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

class QThread;

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

// Which edges of a model are drawn over it.
enum class EdgeMode
{
    Off,
    All,   // every edge of the mesh
    Sharp, // the boundary and the edges where the surface bends by more than 45 degrees
    Wireframe, // every edge, and no surface; the lines fade with the distance
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

    // The color behind the model (sRGB); the top of the gradient when that is
    // on. The default is XjetStudio's deep blue.
    void setBackground(const QColor& color);
    QColor background() const { return m_Background; }
    static QColor defaultBackground() { return QColor(0x33, 0x33, 0x66); }
    // The bottom of the gradient, which is used when it is on.
    void setBackgroundBottom(const QColor& color);
    QColor backgroundBottom() const { return m_BackgroundBottom; }
    static QColor defaultBackgroundBottom() { return QColor(0x11, 0x11, 0x22); }
    void setGradientBackground(bool enabled);
    bool isGradientBackground() const { return m_GradientBackground; }

    // The color of the STL and 3MF model shown (linear rgb), at once; later
    // models are loaded with their own color argument.
    void setModelColor(const vsg::vec4& diffuse);

    // "Use PBR shader": STL and 3MF models are drawn with the physically based
    // shaders (their color is the base color), and the metallic and roughness
    // factors of glTF materials are replaced. Applies to the model shown and
    // to the ones shown later; off, the Phong shaders and the file's own values.
    void setMaterialOverride(bool enabled, float metallic, float roughness);

    // Draws the edges of the STL and 3MF model over it (as XjetStudio's
    // View > Edges), whatever shader shades it. They are computed in the
    // background the first time they are asked for, for each model. The other
    // formats are not drawn with edges.
    void setEdgeMode(EdgeMode mode);
    EdgeMode edgeMode() const { return m_EdgeMode; }
    // The color (sRGB) and the width in pixels, changed live. The width is 1
    // where the device has no wide lines.
    void setEdgeColor(const QColor& color);
    QColor edgeColor() const { return m_EdgeColor; }
    static QColor defaultEdgeColor() { return QColor(0, 0, 0); }
    void setEdgeWidth(double width);
    double edgeWidth() const { return m_EdgeWidth; }
    static double defaultEdgeWidth() { return 1.0; }
    // The widest line the device draws.
    double maxEdgeWidth() const;

    // Plays the animations of the model shown (glTF, ...); none for the others.
    AnimationPlayer* animationPlayer() const { return m_AnimationPlayer; }

    void toggleProjection() { setOrthographic(!m_Orthographic); }
    void setOrthographic(bool orthographic);
    bool isOrthographic() const { return m_Orthographic; }

    // For the thumbnailer, which renders on the same (one and only) device.
    vsg::ref_ptr<vsg::Device> device() const;
    vsg::ref_ptr<vsg::Options> options() const { return m_Options; }

  signals:
    void projectionChanged(bool orthographic);
    // The edges of the model are being computed (true), or are done.
    void edgesBusy(bool busy);
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
    // Pushes the background colors to whatever draws them.
    void applyBackground();
    // The PBR materials of m_Model, made updatable and remembered as authored.
    void collectMaterials();
    void applyMaterialOverride();
    // Off while glTF materials are lit by the environment; other models (assimp's Phong materials) still need them.
    float studioLightsScale() const { return m_OverrideMaterial && !m_PbrMaterials.empty() ? 0.0f : 1.0f; }
    // The STL/3MF model: its PBR material's values, and the shader that is in use.
    void updateCompactMaterial(bool rebuild);
    // Forgets the edges of the previous model, and computes those of the new one if they are shown.
    void resetEdges();
    void startEdgeComputation();
    void applyEdgeWidth();
    // The wireframe is on for this model: the surface is left out, from the
    // start, while its edges are still being computed (a blank view, rather
    // than the solid model that is replaced a moment later).
    bool wireframeActive() const { return m_EdgeMode == EdgeMode::Wireframe && m_Compact && !m_EdgesFailed; }
    // Per frame: the depth range of the wireframe's fade follows the camera.
    void updateEdgeFade();
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
    vsg::ref_ptr<vsg::Visitor> m_EdgeFadeUpdater;
    vsg::ref_ptr<vsg::RenderGraph> m_RenderGraph;
    vsg::ref_ptr<vsg::Node> m_Model;
    vsg::ref_ptr<vsg::Node> m_StudioLights; // the lights of the scene graph
    vsg::dbox m_ModelBounds;
    CompactSource m_Compact; // of the model, when it is one
    QColor m_Background = defaultBackground();
    QColor m_BackgroundBottom = defaultBackgroundBottom();
    bool m_GradientBackground = false;
    struct PbrMaterialState
    {
        vsg::ref_ptr<vsg::PbrMaterialValue> material;
        float metallic, roughness; // as authored
        float specularAlpha;       // ditto: the alpha of the specularFactor tells the shader to light with the environment
    };
    std::vector<PbrMaterialState> m_PbrMaterials;
    bool m_OverrideMaterial = false;
    float m_OverrideMetallic = 0.0f, m_OverrideRoughness = 0.5f;
    vsg::ref_ptr<GradientBackground> m_Gradient; // while it is in the scene graph
    bool m_AmbientOcclusion = false;
    vsg::ref_ptr<AmbientOcclusion> m_Occlusion; // while it is in the scene graph
    vsg::ref_ptr<AmbientOcclusionUpdater> m_OcclusionUpdater;
    // The size of the window in pixels, as of the last resize; the occlusion's
    // images are made at that size, and made anew (after a pause) when it changes.
    VkExtent2D m_PixelExtent{0, 0};
    QTimer m_OcclusionResizeTimer;
    EdgeMode m_EdgeMode = EdgeMode::Off;
    QColor m_EdgeColor = defaultEdgeColor();
    double m_EdgeWidth = defaultEdgeWidth();
    EdgeStyle m_EdgeStyle;
    EdgeSet m_Edges; // of the model, once computed
    bool m_EdgesFailed = false; // computing them failed: the wireframe falls back to the surface
    QThread* m_EdgeThread = nullptr; // computing them
    std::shared_ptr<std::atomic<bool>> m_EdgeCancel;
    int m_EdgeGeneration = 0; // counts the models, so that a late result is known to be stale
    AnimationPlayer* m_AnimationPlayer = nullptr; // owned by Qt
    vsg::dvec3 m_FitCenter{0.0, 0.0, 0.0};
    double m_FitRadius = 1.0;
    bool m_Orthographic = false;
    NavigationStyle m_NavigationStyle = NavigationStyle::Netfabb;
};

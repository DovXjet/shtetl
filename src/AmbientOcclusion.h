#pragma once

#include <vsg/all.h>

#include <vector>

// Which occlusion the passes compute: the Crytek-style hemisphere SSAO of
// XjetStudio, CORTAO (circular occluder ray traced AO, compute shaders), or both.
enum class OcclusionAlgorithm
{
  Ssao,
  Cortao,
  // Both, the darker of the two at each pixel: CORTAO for the creases it sees, and the
  // SSAO for the steps in depth (walls seen edge-on), which CORTAO does not darken.
  Hybrid
};

// Screen space ambient occlusion, as XjetStudio does it (ssao-pipeline.cpp),
// reduced to what shtetl needs. Offscreen passes, in this order:
//
//   G-buffer  the model, drawn into position / normal / lit color targets
//   SSAO      the occlusion of each pixel, from the position and normal
//             (CORTAO: a prepass and a main pass, both compute shaders)
//   blur      removes the noise of the SSAO pass
//
// the composition, which darkens the lit color by the occlusion, and FXAA
// (the multisampling of the window does not reach these passes), whose
// fullscreen view is the one in the window. The caller puts renderGraphs()
// into the command graph ahead of the window's render graph, and
// compositionView() into the window's.
//
// Everything depends on the size of the window: make a new one when it changes.
class AmbientOcclusion : public vsg::Inherit<vsg::Object, AmbientOcclusion>
{
  public:
    // `scene` is what the G-buffer pass draws; `camera` is the one of the
    // window, whose view and projection the passes follow. `radius` is the
    // reach of the occlusion in the units of the model.
    AmbientOcclusion(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> scene, VkExtent2D extent, double radius, vsg::vec3 backgroundTop, vsg::vec3 backgroundBottom,
                     OcclusionAlgorithm algorithm = OcclusionAlgorithm::Ssao);

    // The passes, in the order of the command graph: render graphs, and for
    // CORTAO also the compute dispatches and barriers between them.
    const std::vector<vsg::ref_ptr<vsg::Node>>& renderGraphs() const { return m_Passes; }
    OcclusionAlgorithm algorithm() const { return m_Algorithm; }
    vsg::ref_ptr<vsg::View> compositionView() const { return m_CompositionView; }
    // The camera of the composition: its viewport has to follow the window.
    vsg::ref_ptr<vsg::Camera> compositionCamera() const { return m_CompositionCamera; }
    VkExtent2D extent() const { return m_Extent; }

    // The colors (sRGB) of the background, where there is no geometry: a
    // vertical gradient, solid when they are the same.
    void setBackground(const vsg::vec3& top, const vsg::vec3& bottom);

    // Where there is no geometry, transparent black instead of the background
    // colors: for rendering to an image (the thumbnails). The target holds the
    // colors premultiplied by their alpha.
    void setTransparentBackground(bool transparent);

    // The weight of the darkening: 0 is none, 1 is all of it.
    void setStrength(float strength);
    // Copies the camera's projection into the passes. Called every frame, as
    // the projection changes with the window and the ortho/perspective switch.
    void updateProjection();

  private:
    // The CORTAO passes (prepass, main pass, and the barriers between them), appended
    // to m_Passes; returns the occlusion they write, for the blur to read.
    vsg::ref_ptr<vsg::ImageInfo> createCortaoPasses(vsg::ref_ptr<vsg::Device> device, const std::vector<vsg::ref_ptr<vsg::ImageInfo>>& gbuffer);
    void updateCortaoParams();

    struct SsaoParams
    {
        vsg::mat4 projection;
        float radius = 1.0f;
        float biasFraction = 0.03f;
        float pad0 = 0.0f, pad1 = 0.0f;
    };
    struct CompositionParams
    {
        float strength = 0.5f;
        float transparentBackground = 0.0f;
        float pad1 = 0.0f, pad2 = 0.0f;
        vsg::vec4 background;
        vsg::vec4 backgroundBottom;
    };

    vsg::ref_ptr<vsg::Camera> m_Camera;
    VkExtent2D m_Extent;
    vsg::ref_ptr<vsg::Value<SsaoParams>> m_SsaoParams;
    vsg::ref_ptr<vsg::Value<CompositionParams>> m_CompositionParams;
    OcclusionAlgorithm m_Algorithm;
    std::vector<vsg::ref_ptr<vsg::Node>> m_Passes;
    // CORTAO: the parameters of the main pass (they follow the projection), and
    // the choice between its perspective and orthographic variants.
    struct CortaoMainParams // CORTAOMainPassParameters
    {
        uint32_t packedResolution = 0;
        uint32_t randomSeed = 0;
        float normalBias = 0.0f;
        float radius = 1.0f;
        vsg::vec4 unproject;
        vsg::vec4 projection;
        float maxApparentCircleRadiusCoefficient = 0.0f;
        float mipBias = 0.0f;
        float inverseMaxSlopeCompensation = 0.0f;
        float backfaceThreshold = 0.0f;
        float backfaceBoost = 0.0f;
        uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
    };
    vsg::ref_ptr<vsg::Value<CortaoMainParams>> m_CortaoParams;
    vsg::ref_ptr<vsg::Switch> m_CortaoVariants;
    double m_Radius = 1.0;
    vsg::ref_ptr<vsg::View> m_CompositionView;
    vsg::ref_ptr<vsg::Camera> m_CompositionCamera;
};

// The background of the plain view: a vertical gradient (sRGB colors) drawn
// before the model, in a view of its own.
class GradientBackground : public vsg::Inherit<vsg::Object, GradientBackground>
{
  public:
    GradientBackground(VkExtent2D extent, VkSampleCountFlagBits samples, const vsg::vec3& top, const vsg::vec3& bottom);

    vsg::ref_ptr<vsg::View> view() const { return m_View; }
    // Its viewport has to follow the window.
    vsg::ref_ptr<vsg::Camera> camera() const { return m_Camera; }
    void setColors(const vsg::vec3& top, const vsg::vec3& bottom);

  private:
    struct Params
    {
        vsg::vec4 top, bottom;
    };
    vsg::ref_ptr<vsg::Value<Params>> m_Params;
    vsg::ref_ptr<vsg::Camera> m_Camera;
    vsg::ref_ptr<vsg::View> m_View;
};

// Per frame, until removed from the viewer's handlers: keeps the projection of
// the passes in step with the camera's.
class AmbientOcclusionUpdater : public vsg::Inherit<vsg::Visitor, AmbientOcclusionUpdater>
{
  public:
    vsg::ref_ptr<AmbientOcclusion> occlusion;
    void apply(vsg::FrameEvent&) override
    {
        if (occlusion) occlusion->updateProjection();
    }
};

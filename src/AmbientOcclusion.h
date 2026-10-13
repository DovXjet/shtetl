#pragma once

#include <vsg/all.h>

#include <vector>

// Screen space ambient occlusion, as XjetStudio does it (ssao-pipeline.cpp),
// reduced to what shtetl needs. Offscreen passes, in this order:
//
//   G-buffer  the model, drawn into position / normal / lit color targets
//   SSAO      the occlusion of each pixel, from the position and normal
//   blur      removes the noise of the SSAO pass
//
// and the composition, a fullscreen view in the window that darkens the lit
// color by the occlusion. The caller puts renderGraphs() into the command
// graph ahead of the window's render graph, and compositionView() into the
// window's.
//
// Everything depends on the size of the window: make a new one when it changes.
class AmbientOcclusion : public vsg::Inherit<vsg::Object, AmbientOcclusion>
{
  public:
    // `scene` is what the G-buffer pass draws; `camera` is the one of the
    // window, whose view and projection the passes follow. `radius` is the
    // reach of the occlusion in the units of the model.
    AmbientOcclusion(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> scene, VkExtent2D extent, double radius);

    std::vector<vsg::ref_ptr<vsg::RenderGraph>> renderGraphs() const { return {m_GBufferGraph, m_SsaoGraph, m_BlurGraph}; }
    vsg::ref_ptr<vsg::View> compositionView() const { return m_CompositionView; }
    // The camera of the composition: its viewport has to follow the window.
    vsg::ref_ptr<vsg::Camera> compositionCamera() const { return m_CompositionCamera; }
    VkExtent2D extent() const { return m_Extent; }

    // The weight of the darkening: 0 is none, 1 is all of it.
    void setStrength(float strength);
    // Copies the camera's projection into the passes. Called every frame, as
    // the projection changes with the window and the ortho/perspective switch.
    void updateProjection();

  private:
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
        float pad0 = 0.0f, pad1 = 0.0f, pad2 = 0.0f;
    };

    vsg::ref_ptr<vsg::Camera> m_Camera;
    VkExtent2D m_Extent;
    vsg::ref_ptr<vsg::Value<SsaoParams>> m_SsaoParams;
    vsg::ref_ptr<vsg::Value<CompositionParams>> m_CompositionParams;
    vsg::ref_ptr<vsg::RenderGraph> m_GBufferGraph, m_SsaoGraph, m_BlurGraph;
    vsg::ref_ptr<vsg::View> m_CompositionView;
    vsg::ref_ptr<vsg::Camera> m_CompositionCamera;
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

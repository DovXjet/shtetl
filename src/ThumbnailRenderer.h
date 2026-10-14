#pragma once

#include "CompactMesh.h"

#include <vsg/all.h>

#include <QImage>

// Renders a scene graph off-screen into an RGBA image, on a given device
// (adapted from XjetStudio's VsgThumbnailMaker). Not thread-safe; use it from
// one thread - the thumbnail worker thread.
class ThumbnailRenderer
{
  public:
    // The device is the viewport's: this build of VSG allows only one.
    void setDevice(vsg::ref_ptr<vsg::Device> device);
    void setExtent(const VkExtent2D& extent) { m_Extent = extent; }

    // The RGBA image of the scene, whose bounds are given (the scene may hold
    // packed vertices that vsg::ComputeBounds can't read), seen from a fixed
    // 3/4 viewpoint, or null. The image is sRGB encoded, as the viewer's is.
    // With `environmentLit` the scene's materials take their light from the
    // environment (see lightWithEnvironment()), and the studio lights are off.
    // With `compact` (an STL or 3MF) the creases are darkened by ambient
    // occlusion, as in the viewer; the image is then rendered at twice the size
    // (antialiased by FXAA, not multisampling), for thumbnailImage() to reduce.
    vsg::ref_ptr<vsg::Data> render(vsg::ref_ptr<vsg::Node> scene, const vsg::dbox& bounds, bool environmentLit = false, const CompactSource* compact = nullptr);

  private:
    vsg::ref_ptr<vsg::Device> m_Device;
    VkExtent2D m_Extent = {256, 256};
    VkSampleCountFlagBits m_Samples = VK_SAMPLE_COUNT_8_BIT; // antialiasing
    int m_QueueFamily = 0;
};

// The image of ThumbnailRenderer::render() as a QImage that owns its pixels
// (the data is mapped GPU memory). The edges of the model are antialiased
// against transparent black, so the colors of their partly covered pixels are
// the coverage times the model's, in linear light; this puts back the model's
// colors there, as a straight-alpha image has them (no dark or colored halo).
// With a `size` the image is scaled down to it, if it is larger.
QImage thumbnailImage(const vsg::Data& data, const QSize& size = {});

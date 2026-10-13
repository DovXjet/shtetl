#pragma once

#include <vsg/all.h>

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
    // 3/4 viewpoint, or null.
    vsg::ref_ptr<vsg::Data> render(vsg::ref_ptr<vsg::Node> scene, const vsg::dbox& bounds);

  private:
    vsg::ref_ptr<vsg::Device> m_Device;
    VkExtent2D m_Extent = {256, 256};
    VkSampleCountFlagBits m_Samples = VK_SAMPLE_COUNT_8_BIT; // antialiasing
    int m_QueueFamily = 0;
};

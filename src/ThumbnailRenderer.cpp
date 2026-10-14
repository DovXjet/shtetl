#include "ThumbnailRenderer.h"

#include "AmbientOcclusion.h"
#include "StudioLights.h"

#include <vsg/all.h>

#include <algorithm>
#include <cmath>
#include <cstring>

static vsg::ref_ptr<vsg::ImageView> createColorImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, VkFormat imageFormat, VkSampleCountFlagBits samples)
{
    auto colorImage = vsg::Image::create();
    colorImage->imageType = VK_IMAGE_TYPE_2D;
    colorImage->format = imageFormat;
    colorImage->extent = VkExtent3D{extent.width, extent.height, 1};
    colorImage->mipLevels = 1;
    colorImage->arrayLayers = 1;
    colorImage->samples = samples;
    colorImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImage->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorImage->flags = 0;
    colorImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    return vsg::createImageView(device, colorImage, VK_IMAGE_ASPECT_COLOR_BIT);
}

static vsg::ref_ptr<vsg::ImageView> createDepthImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, VkFormat depthFormat, VkSampleCountFlagBits samples)
{
    auto depthImage = vsg::Image::create();
    depthImage->imageType = VK_IMAGE_TYPE_2D;
    depthImage->extent = VkExtent3D{extent.width, extent.height, 1};
    depthImage->mipLevels = 1;
    depthImage->arrayLayers = 1;
    depthImage->samples = samples;
    depthImage->format = depthFormat;
    depthImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImage->usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    depthImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImage->flags = 0;
    depthImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    return vsg::createImageView(device, depthImage, vsg::computeAspectFlagsForFormat(depthFormat));
}

static std::pair<vsg::ref_ptr<vsg::Commands>, vsg::ref_ptr<vsg::Image>> createColorCapture(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, vsg::ref_ptr<vsg::Image> sourceImage, VkFormat sourceImageFormat)
{
    auto width = extent.width;
    auto height = extent.height;

    auto physicalDevice = device->getPhysicalDevice();

    VkFormat targetImageFormat = sourceImageFormat;

    //
    // 1) Check to see if Blit is supported.
    //
    VkFormatProperties srcFormatProperties;
    vkGetPhysicalDeviceFormatProperties(*(physicalDevice), sourceImageFormat, &srcFormatProperties);

    VkFormatProperties destFormatProperties;
    // The blit converts between the formats' encodings: an sRGB image has to
    // go to an sRGB one, or the colors would be linear again.
    const VkFormat blitFormat = sourceImageFormat == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    vkGetPhysicalDeviceFormatProperties(*(physicalDevice), blitFormat, &destFormatProperties);

    bool supportsBlit = ((srcFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) &&
        ((destFormatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0);

    if (supportsBlit)
    {
        // we can automatically convert the image format when blit, so take advantage of it to ensure RGBA
        targetImageFormat = blitFormat;
    }

    //std::cout<<"supportsBlit = "<<supportsBlit<<std::endl;

    //
    // 2) create image to write to
    //
    auto destinationImage = vsg::Image::create();
    destinationImage->imageType = VK_IMAGE_TYPE_2D;
    destinationImage->format = targetImageFormat;
    destinationImage->extent.width = width;
    destinationImage->extent.height = height;
    destinationImage->extent.depth = 1;
    destinationImage->arrayLayers = 1;
    destinationImage->mipLevels = 1;
    destinationImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    destinationImage->samples = VK_SAMPLE_COUNT_1_BIT;
    destinationImage->tiling = VK_IMAGE_TILING_LINEAR;
    destinationImage->usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    destinationImage->compile(device);

    auto deviceMemory = vsg::DeviceMemory::create(device, destinationImage->getMemoryRequirements(device->deviceID), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    destinationImage->bind(deviceMemory, 0);

    //
    // 3) create command buffer and submit to graphics queue
    //
    auto commands = vsg::Commands::create();

    // 3.a) transition destinationImage to transfer destination initialLayout
    auto transitionDestinationImageToDestinationLayoutBarrier = vsg::ImageMemoryBarrier::create(
        0,                                                             // srcAccessMask
        VK_ACCESS_TRANSFER_WRITE_BIT,                                  // dstAccessMask
        VK_IMAGE_LAYOUT_UNDEFINED,                                     // oldLayout
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,                          // newLayout
        VK_QUEUE_FAMILY_IGNORED,                                       // srcQueueFamilyIndex
        VK_QUEUE_FAMILY_IGNORED,                                       // dstQueueFamilyIndex
        destinationImage,                                              // image
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} // subresourceRange
        );

    // 3.b) transition swapChainImage from present to transfer source initialLayout
    auto transitionSourceImageToTransferSourceLayoutBarrier = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_MEMORY_READ_BIT,                                     // srcAccessMask
        VK_ACCESS_TRANSFER_READ_BIT,                                   // dstAccessMask
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,                               // oldLayout
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,                          // newLayout
        VK_QUEUE_FAMILY_IGNORED,                                       // srcQueueFamilyIndex
        VK_QUEUE_FAMILY_IGNORED,                                       // dstQueueFamilyIndex
        sourceImage,                                                   // image
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} // subresourceRange
        );

    auto cmd_transitionForTransferBarrier = vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,                       // srcStageMask
        VK_PIPELINE_STAGE_TRANSFER_BIT,                       // dstStageMask
        0,                                                    // dependencyFlags
        transitionDestinationImageToDestinationLayoutBarrier, // barrier
        transitionSourceImageToTransferSourceLayoutBarrier    // barrier
        );

    commands->addChild(cmd_transitionForTransferBarrier);

    if (supportsBlit)
    {
        // 3.c.1) if blit using vkCmdBlitImage
        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[0] = VkOffset3D{0, 0, 0};
        region.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[0] = VkOffset3D{0, 0, 0};
        region.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(width), static_cast<int32_t>(height), 1};

        auto blitImage = vsg::BlitImage::create();
        blitImage->srcImage = sourceImage;
        blitImage->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blitImage->dstImage = destinationImage;
        blitImage->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blitImage->regions.push_back(region);
        blitImage->filter = VK_FILTER_NEAREST;

        commands->addChild(blitImage);
    }
    else
    {
        // 3.c.2) else use vkCmdCopyImage

        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent.width = width;
        region.extent.height = height;
        region.extent.depth = 1;

        auto copyImage = vsg::CopyImage::create();
        copyImage->srcImage = sourceImage;
        copyImage->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyImage->dstImage = destinationImage;
        copyImage->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyImage->regions.push_back(region);

        commands->addChild(copyImage);
    }

    // 3.d) transition destination image from transfer destination layout to general layout to enable mapping to image DeviceMemory
    auto transitionDestinationImageToMemoryReadBarrier = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_WRITE_BIT,                                  // srcAccessMask
        VK_ACCESS_MEMORY_READ_BIT,                                     // dstAccessMask
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,                          // oldLayout
        VK_IMAGE_LAYOUT_GENERAL,                                       // newLayout
        VK_QUEUE_FAMILY_IGNORED,                                       // srcQueueFamilyIndex
        VK_QUEUE_FAMILY_IGNORED,                                       // dstQueueFamilyIndex
        destinationImage,                                              // image
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} // subresourceRange
        );

    // 3.e) transition swap chain image back to present
    auto transitionSourceImageBackToPresentBarrier = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_READ_BIT,                                   // srcAccessMask
        VK_ACCESS_MEMORY_READ_BIT,                                     // dstAccessMask
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,                          // oldLayout
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,                               // newLayout
        VK_QUEUE_FAMILY_IGNORED,                                       // srcQueueFamilyIndex
        VK_QUEUE_FAMILY_IGNORED,                                       // dstQueueFamilyIndex
        sourceImage,                                                   // image
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} // subresourceRange
        );

    auto cmd_transitionFromTransferBarrier = vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,                // srcStageMask
        VK_PIPELINE_STAGE_TRANSFER_BIT,                // dstStageMask
        0,                                             // dependencyFlags
        transitionDestinationImageToMemoryReadBarrier, // barrier
        transitionSourceImageBackToPresentBarrier      // barrier
        );

    commands->addChild(cmd_transitionFromTransferBarrier);

    return {commands, destinationImage};
}


void ThumbnailRenderer::setDevice(vsg::ref_ptr<vsg::Device> device)
{
  m_Device = device;
  // The queue family the render graph is submitted to.
  auto [physicalDevice, queueFamily] = device->getInstance()->getPhysicalDeviceAndQueueFamily(VK_QUEUE_GRAPHICS_BIT);
  m_QueueFamily = queueFamily;
}

QImage thumbnailImage(const vsg::Data& data, const QSize& size)
{
    QImage view(static_cast<const uchar*>(data.dataPointer()), int(data.width()), int(data.height()), QImage::Format_RGBA8888);
    QImage image = view.copy();
    if (size.isValid() && image.width() > size.width() && image.width() % size.width() == 0 && image.height() % size.height() == 0 &&
        image.width() / size.width() == image.height() / size.height())
    {
        // A box filter, in linear light: the colors are premultiplied by the
        // coverage, and averaging their sRGB codes would darken the silhouette.
        const int factor = image.width() / size.width();
        auto toLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        auto toCode = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
        QImage reduced(size, QImage::Format_RGBA8888);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x)
            {
                float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int dy = 0; dy < factor; ++dy)
                {
                    const uchar* row = image.constScanLine(y * factor + dy) + 4 * x * factor;
                    for (int dx = 0; dx < factor; ++dx)
                    {
                        for (int k = 0; k < 3; ++k) sum[k] += toLinear(row[4 * dx + k] / 255.0f);
                        sum[3] += row[4 * dx + 3] / 255.0f; // the alpha is linear
                    }
                }
                uchar* pixel = reduced.scanLine(y) + 4 * x;
                const float n = float(factor * factor);
                for (int k = 0; k < 3; ++k) pixel[k] = static_cast<uchar>(std::lround(toCode(sum[k] / n) * 255.0f));
                pixel[3] = static_cast<uchar>(std::lround(sum[3] / n * 255.0f));
            }
        image = reduced;
    }

    auto decode = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    auto encode = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
    for (int y = 0; y < image.height(); ++y)
    {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
        {
            uchar* pixel = row + 4 * x;
            const int alpha = pixel[3];
            if (alpha == 0 || alpha == 255) continue;
            const float coverage = alpha / 255.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float linear = std::min(decode(pixel[k] / 255.0f) / coverage, 1.0f);
                pixel[k] = static_cast<uchar>(std::lround(encode(linear) * 255.0f));
            }
        }
    }
    return image;
}

vsg::ref_ptr<vsg::Data> ThumbnailRenderer::render(vsg::ref_ptr<vsg::Node> Scene, const vsg::dbox& bounds, bool environmentLit, const CompactSource* compact)
{
    const bool occluded = compact && *compact;
    const VkExtent2D extent{m_Extent.width * (occluded ? 2u : 1u), m_Extent.height * (occluded ? 2u : 1u)};
    // compute the bounds of the scene graph to help position camera
    if (!bounds.valid()) return {};
    const vsg::dbox& b = bounds;
    vsg::dvec3 centre = (b.min + b.max) * 0.5;
    double radius = vsg::length(b.max - b.min);
    double nearFarRatio = 0.001;
    // sRGB, so that the (linear) colors of the shaders are encoded as in the
    // viewer's swapchain; UNORM stored them as they were, which is far too dark.
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // set up the camera
    // Place the eye far enough that the bounding sphere fits the (vertical) field
    // of view; the view is square, so it fits horizontally too.
    constexpr double fovDegrees = 30.0;
    double sphereRadius = radius * 0.5;
    double distance = sphereRadius / std::sin(vsg::radians(fovDegrees * 0.5)) * 1.1;
    auto lookAt = vsg::LookAt::create(
        centre + vsg::normalize(vsg::dvec3(-0.5, -1.0, 0.5)) * distance,
        centre,
        vsg::dvec3(0.0, 0.0, 1.0));


    vsg::ref_ptr<vsg::ProjectionMatrix> perspective = vsg::Perspective::create(
        fovDegrees,
        1.0 * extent.width / extent.height,
        nearFarRatio * radius, distance + sphereRadius * 1.5);

    auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(extent));

    // set up the RenderGraph to manage the rendering
    // With occlusion the passes of AmbientOcclusion draw the model, one sample
    // per pixel; the render graph then only draws their composition.
    const VkSampleCountFlagBits samples = occluded ? VK_SAMPLE_COUNT_1_BIT : m_Samples;
    auto colorImageView = createColorImageView(m_Device, extent, imageFormat, VK_SAMPLE_COUNT_1_BIT);
    auto depthImageView = createDepthImageView(m_Device, extent, depthFormat, VK_SAMPLE_COUNT_1_BIT);
    auto renderPass = vsg::createMultisampledRenderPass(m_Device, imageFormat, depthFormat, samples, true);
    vsg::ref_ptr<vsg::Framebuffer> framebuffer;
    if (samples == VK_SAMPLE_COUNT_1_BIT)
    {
        framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{colorImageView, depthImageView}, extent.width, extent.height, 1);
    }
    else
    {
        auto msaa_colorImageView = createColorImageView(m_Device, extent, imageFormat, samples);
        auto msaa_depthImageView = createDepthImageView(m_Device, extent, depthFormat, samples);
        framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{msaa_colorImageView, colorImageView, msaa_depthImageView, depthImageView}, extent.width, extent.height, 1);
    }

    // create support for copying the color buffer
    auto [colorBufferCapture, copiedColorBuffer] = createColorCapture(m_Device, extent, colorImageView->image, imageFormat);

    auto renderGraph = vsg::RenderGraph::create();

    renderGraph->framebuffer = framebuffer;
    renderGraph->renderArea.offset = {0, 0};
    renderGraph->renderArea.extent = extent;
    renderGraph->setClearValues({{0.0f, 0.0f, 0.0f, 0.0f}}, VkClearDepthStencilValue{0.0f, 0});

    vsg::ref_ptr<AmbientOcclusion> occlusion;
    if (occluded)
    {
        // As the viewer does it (Viewport3D::createScene); the radius is a
        // fraction of the bounding sphere's, as there. The shading of the
        // G-buffer's lit color is that of the compact mesh shader.
        auto gbufferScene = vsg::Group::create();
        gbufferScene->addChild(makeCompactGBufferNode(*compact));
        occlusion = AmbientOcclusion::create(m_Device, camera, gbufferScene, extent, sphereRadius * 0.12, vsg::vec3(0.0f, 0.0f, 0.0f), vsg::vec3(0.0f, 0.0f, 0.0f));
        occlusion->setStrength(0.85f);
        occlusion->setTransparentBackground(true);
        renderGraph->addChild(occlusion->compositionView());
    }
    else
    {
        auto view = vsg::View::create(camera);
        auto studioLights = createStudioLights();
        if (environmentLit) scaleStudioLights(studioLights, 0.0f);
        view->addChild(studioLights);
        view->addChild(Scene);
        renderGraph->addChild(view);
    }

    vsg::CommandGraphs commandGraphs;
    auto commandGraph = vsg::CommandGraph::create(m_Device, m_QueueFamily);
    if (occlusion)
        for (auto& graph : occlusion->renderGraphs()) commandGraph->addChild(graph);
    commandGraph->addChild(renderGraph);
    commandGraphs.push_back(commandGraph);
    if (colorBufferCapture)
        commandGraph->addChild(colorBufferCapture);

    // create the viewer
    auto viewer = vsg::Viewer::create();

    viewer->assignRecordAndSubmitTaskAndPresentation(commandGraphs);

    viewer->compile();

    // Render
    uint64_t waitTimeout = 1999999999; // 1second in nanoseconds.
    vsg::ref_ptr<vsg::Data> imageData;

    if (viewer->advanceToNextFrame())
    {
        viewer->handleEvents();

        viewer->update();

        viewer->recordAndSubmit();

        if (copiedColorBuffer)
        {
            // wait for completion.
            // A timed-out wait would leave the capture unwritten (all zeros), which
            // would be cached as a black thumbnail.
            if (viewer->waitForFences(0, waitTimeout) != VK_SUCCESS) return {};

            VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            VkSubresourceLayout subResourceLayout;
            vkGetImageSubresourceLayout(*m_Device, copiedColorBuffer->vk(m_Device->deviceID), &subResource, &subResourceLayout);

            auto deviceMemory = copiedColorBuffer->getDeviceMemory(m_Device->deviceID);

            size_t destRowWidth = extent.width * sizeof(vsg::ubvec4);
            if (destRowWidth == subResourceLayout.rowPitch)
            {
                // Map the buffer memory and assign as a vec4Array2D that will automatically unmap itself on destruction.
                imageData = vsg::MappedData<vsg::ubvec4Array2D>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, extent.width, extent.height);
            }
            else
            {
                // Map the buffer memory and assign as a ubyteArray that will automatically unmap itself on destruction.
                // A ubyteArray is used as the graphics buffer memory is not contiguous like vsg::Array2D, so map to a flat buffer first then copy to Array2D.
                auto mappedData = vsg::MappedData<vsg::ubyteArray>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, subResourceLayout.rowPitch * extent.height);
                imageData = vsg::ubvec4Array2D::create(extent.width, extent.height, vsg::Data::Properties{imageFormat});
                for (uint32_t row = 0; row < extent.height; ++row)
                {
                    std::memcpy(imageData->dataPointer(row * extent.width), mappedData->dataPointer(row * subResourceLayout.rowPitch), destRowWidth);
                }
            }
        }
    }
    return imageData;

}


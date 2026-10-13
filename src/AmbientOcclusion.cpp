#include "AmbientOcclusion.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <stdexcept>

namespace
{
  constexpr uint32_t NoiseDim = 8;
  constexpr uint32_t KernelSize = 64;

  // The shaders are compiled from source when first used, and kept: a new
  // scene graph (every model shown) would otherwise compile them again.
  vsg::ref_ptr<vsg::ShaderStage> shader(VkShaderStageFlagBits stage, const char* resource)
  {
    static std::map<std::string, vsg::ref_ptr<vsg::ShaderStage>> cache;
    auto& entry = cache[resource];
    if (!entry)
    {
      QFile file(resource);
      if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(std::string("Missing resource ") + resource);
      entry = vsg::ShaderStage::create(stage, "main", file.readAll().toStdString());
    }
    return entry;
  }

  // An offscreen render graph with the given color targets, which the later
  // passes sample, and a depth buffer of its own. Reverse z, as the window's.
  vsg::ref_ptr<vsg::RenderGraph> createOffscreenGraph(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& extent, const std::vector<VkFormat>& formats,
                                                      std::vector<vsg::ref_ptr<vsg::ImageInfo>>& images)
  {
    const VkExtent3D attachmentExtent{extent.width, extent.height, 1};
    std::vector<vsg::ref_ptr<vsg::ImageView>> views;
    images.clear();
    for (VkFormat format : formats)
    {
      auto image = vsg::Image::create();
      image->imageType = VK_IMAGE_TYPE_2D;
      image->format = format;
      image->extent = attachmentExtent;
      image->mipLevels = 1;
      image->arrayLayers = 1;
      image->samples = VK_SAMPLE_COUNT_1_BIT;
      image->tiling = VK_IMAGE_TILING_OPTIMAL;
      image->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      image->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      auto view = vsg::createImageView(device, image, VK_IMAGE_ASPECT_COLOR_BIT);
      views.push_back(view);

      auto sampler = vsg::Sampler::create();
      sampler->magFilter = VK_FILTER_NEAREST;
      sampler->minFilter = VK_FILTER_NEAREST;
      sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sampler->addressModeU = sampler->addressModeV = sampler->addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler->maxAnisotropy = 1.0f;
      sampler->maxLod = 0.0f;
      images.push_back(vsg::ImageInfo::create(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }

    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    auto depthImage = vsg::Image::create();
    depthImage->imageType = VK_IMAGE_TYPE_2D;
    depthImage->extent = attachmentExtent;
    depthImage->mipLevels = 1;
    depthImage->arrayLayers = 1;
    depthImage->samples = VK_SAMPLE_COUNT_1_BIT;
    depthImage->format = depthFormat;
    depthImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImage->usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto depthView = vsg::createImageView(device, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT);

    const size_t count = formats.size();
    vsg::RenderPass::Attachments attachments(count + 1);
    for (size_t i = 0; i < count; ++i)
    {
      attachments[i].format = formats[i];
      attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachments[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    attachments[count].format = depthFormat;
    attachments[count].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[count].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[count].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[count].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[count].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    vsg::RenderPass::Subpasses subpasses(1);
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    for (size_t i = 0; i < count; ++i) subpasses[0].colorAttachments.push_back({static_cast<uint32_t>(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    subpasses[0].depthStencilAttachments.push_back({static_cast<uint32_t>(count), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});

    // The later passes read this one's targets non-locally (the blur and the
    // occlusion look at the neighbours), so no BY_REGION on the way out.
    vsg::RenderPass::Dependencies dependencies(2);
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = 0;

    auto renderPass = vsg::RenderPass::create(device, attachments, subpasses, dependencies);
    std::vector<vsg::ref_ptr<vsg::ImageView>> framebufferViews = views;
    framebufferViews.push_back(depthView);
    auto framebuffer = vsg::Framebuffer::create(renderPass, framebufferViews, extent.width, extent.height, 1);

    auto graph = vsg::RenderGraph::create();
    graph->renderArea.offset = {0, 0};
    graph->renderArea.extent = extent;
    graph->framebuffer = framebuffer;
    graph->clearValues.resize(count + 1);
    for (size_t i = 0; i < count; ++i) graph->clearValues[i].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    graph->clearValues[count].depthStencil = {0.0f, 0};
    return graph;
  }

  // A fullscreen triangle with the given fragment shader, which reads the
  // given images (bindings 0...) and uniform buffers (the descriptors).
  vsg::ref_ptr<vsg::Node> createFullscreenPass(const char* fragmentResource, const std::vector<vsg::ref_ptr<vsg::ImageInfo>>& images,
                                               const std::vector<vsg::ref_ptr<vsg::Descriptor>>& uniforms)
  {
    vsg::ShaderStages stages{shader(VK_SHADER_STAGE_VERTEX_BIT, ":/shaders/ao_fullscreen.vert"), shader(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentResource)};

    vsg::DescriptorSetLayoutBindings bindings;
    vsg::Descriptors descriptors;
    for (uint32_t i = 0; i < images.size(); ++i)
    {
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      descriptors.push_back(vsg::DescriptorImage::create(images[i], i, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
    }
    for (const auto& uniform : uniforms)
    {
      auto buffer = uniform.cast<vsg::DescriptorBuffer>();
      bindings.push_back({buffer->dstBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      descriptors.push_back(uniform);
    }

    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = VK_CULL_MODE_NONE;
    auto depthStencil = vsg::DepthStencilState::create();
    depthStencil->depthTestEnable = VK_FALSE;
    depthStencil->depthWriteEnable = VK_FALSE;
    depthStencil->depthCompareOp = VK_COMPARE_OP_ALWAYS;
    vsg::GraphicsPipelineStates states{
        vsg::VertexInputState::create(vsg::VertexInputState::Bindings{}, vsg::VertexInputState::Attributes{}),
        vsg::InputAssemblyState::create(),
        rasterization,
        vsg::MultisampleState::create(),
        vsg::ColorBlendState::create(),
        depthStencil,
    };

    auto setLayout = vsg::DescriptorSetLayout::create(bindings);
    auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{setLayout}, vsg::PushConstantRanges{});
    auto pipeline = vsg::GraphicsPipeline::create(pipelineLayout, stages, states);
    auto descriptorSet = vsg::DescriptorSet::create(setLayout, descriptors);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(vsg::BindGraphicsPipeline::create(pipeline));
    stateGroup->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, descriptorSet));
    stateGroup->addChild(vsg::Draw::create(3, 1, 0, 0));
    return stateGroup;
  }

  // The camera of a fullscreen pass: the shaders place the triangle
  // themselves, only the viewport matters.
  vsg::ref_ptr<vsg::Camera> createFullscreenCamera(const VkExtent2D& extent)
  {
    auto camera = vsg::Camera::create();
    camera->projectionMatrix = vsg::Orthographic::create(-1.0, 1.0, -1.0, 1.0, 0.0, 1.0);
    camera->viewMatrix = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, -1.0), vsg::dvec3(0.0, 1.0, 0.0));
    camera->viewportState = vsg::ViewportState::create(extent);
    return camera;
  }
} // namespace

AmbientOcclusion::AmbientOcclusion(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> scene, VkExtent2D extent, double radius)
    : m_Camera(camera), m_Extent(extent)
{
  m_SsaoParams = vsg::Value<SsaoParams>::create();
  m_SsaoParams->properties.dataVariance = vsg::DYNAMIC_DATA;
  m_SsaoParams->value().radius = static_cast<float>(radius);
  m_CompositionParams = vsg::Value<CompositionParams>::create();
  m_CompositionParams->properties.dataVariance = vsg::DYNAMIC_DATA;
  updateProjection();

  // The random rotations of the kernel around each normal, tiled over the
  // screen, and the hemisphere of sample offsets, denser near the origin.
  std::default_random_engine random(12345);
  std::uniform_real_distribution<float> uniform(0.0f, 1.0f);

  auto noise = vsg::vec4Array2D::create(NoiseDim, NoiseDim);
  noise->properties.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  for (uint32_t i = 0; i < NoiseDim * NoiseDim; ++i) noise->set(i, vsg::vec4(uniform(random) * 2.0f - 1.0f, uniform(random) * 2.0f - 1.0f, 0.0f, 0.0f));
  auto noiseImage = vsg::Image::create();
  noiseImage->data = noise;
  noiseImage->format = noise->properties.format;
  noiseImage->extent = VkExtent3D{NoiseDim, NoiseDim, 1};
  noiseImage->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  noiseImage->arrayLayers = 1;
  auto noiseSampler = vsg::Sampler::create(); // repeats, so that the noise tiles
  noiseSampler->magFilter = VK_FILTER_NEAREST;
  noiseSampler->minFilter = VK_FILTER_NEAREST;
  auto noiseInfo = vsg::ImageInfo::create(noiseSampler, vsg::ImageView::create(noiseImage), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  auto kernel = vsg::vec4Array::create(KernelSize);
  kernel->properties.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  for (uint32_t i = 0; i < KernelSize; ++i)
  {
    vsg::vec3 sample(uniform(random) * 2.0f - 1.0f, uniform(random) * 2.0f - 1.0f, uniform(random));
    sample = vsg::normalize(sample) * uniform(random);
    const float t = static_cast<float>(i) / static_cast<float>(KernelSize);
    sample *= std::lerp(0.1f, 1.0f, t * t);
    kernel->set(i, vsg::vec4(sample, 0.0f));
  }

  // The G-buffer: view space position (with the z in w), normal, lit color.
  std::vector<vsg::ref_ptr<vsg::ImageInfo>> gbuffer;
  m_GBufferGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, gbuffer);
  auto gbufferCamera = vsg::Camera::create(camera->projectionMatrix, camera->viewMatrix, vsg::ViewportState::create(extent));
  m_GBufferGraph->addChild(vsg::View::create(gbufferCamera, scene));

  std::vector<vsg::ref_ptr<vsg::ImageInfo>> occlusion;
  m_SsaoGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R8_UNORM}, occlusion);
  auto ssaoView = vsg::View::create(createFullscreenCamera(extent),
                                    createFullscreenPass(":/shaders/ao_ssao.frag", {gbuffer[0], gbuffer[1], noiseInfo},
                                                         {vsg::DescriptorBuffer::create(kernel, 3, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                          vsg::DescriptorBuffer::create(m_SsaoParams, 4, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)}));
  m_SsaoGraph->addChild(ssaoView);

  std::vector<vsg::ref_ptr<vsg::ImageInfo>> blurred;
  m_BlurGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R8_UNORM}, blurred);
  m_BlurGraph->addChild(vsg::View::create(createFullscreenCamera(extent), createFullscreenPass(":/shaders/ao_blur.frag", {occlusion[0]}, {})));

  m_CompositionCamera = createFullscreenCamera(extent);
  m_CompositionView = vsg::View::create(
      m_CompositionCamera, createFullscreenPass(":/shaders/ao_composite.frag", {gbuffer[0], gbuffer[2], blurred[0]},
                                                {vsg::DescriptorBuffer::create(m_CompositionParams, 3, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)}));
}

void AmbientOcclusion::setStrength(float strength)
{
  m_CompositionParams->value().strength = std::clamp(strength, 0.0f, 1.0f);
  m_CompositionParams->dirty();
}

void AmbientOcclusion::updateProjection()
{
  m_SsaoParams->value().projection = vsg::mat4(m_Camera->projectionMatrix->transform());
  m_SsaoParams->dirty();
}

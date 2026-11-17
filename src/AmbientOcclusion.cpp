#include "AmbientOcclusion.h"

#include <QFile>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
                                                      std::vector<vsg::ref_ptr<vsg::ImageInfo>>& images, VkFilter filter = VK_FILTER_NEAREST)
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
      sampler->magFilter = filter;
      sampler->minFilter = filter;
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
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; // (CORTAO reads the targets in compute shaders)
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
                                               const std::vector<vsg::ref_ptr<vsg::Descriptor>>& uniforms,
                                               VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT)
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
    auto multisample = vsg::MultisampleState::create();
    multisample->rasterizationSamples = samples;
    vsg::GraphicsPipelineStates states{
        vsg::VertexInputState::create(vsg::VertexInputState::Bindings{}, vsg::VertexInputState::Attributes{}),
        vsg::InputAssemblyState::create(),
        rasterization,
        multisample,
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

  // ---- CORTAO (see resources/shaders/cortao, the MIT licensed reference by Daniel Isheden) ----

  constexpr uint32_t CortaoMaxMips = 8;   // what the prepass writes
  constexpr uint32_t CortaoRayCount = 16; // must match the occluder table
  constexpr uint32_t CortaoTableWidth = 24, CortaoTableHeight = 24, CortaoTableDepth = 16;

  struct CortaoPrepassParams // CORTAOPrepassParameters
  {
    uint32_t packedResolution = 0;
    float depthScale = 1.0f;
    float depthOffset = 0.0f;
    uint32_t randomSeed = 0;
    uint32_t mipCount = 1;
    uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
  };

  std::string resourceText(const char* resource)
  {
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(std::string("Missing resource ") + resource);
    return file.readAll().toStdString();
  }

  // A CORTAO compute shader: its sources are put together here (the reference
  // has no include path in VSG), with the variant's defines in front.
  vsg::ref_ptr<vsg::ShaderStage> cortaoShader(const char* passHeader, const char* wrapper, bool orthographic, uint32_t sampleCount, const char* extraDefines = "")
  {
    static std::map<std::string, vsg::ref_ptr<vsg::ShaderStage>> cache;
    const std::string key = std::string(wrapper) + (orthographic ? ":ortho:" : ":persp:") + std::to_string(sampleCount) + extraDefines;
    auto& entry = cache[key];
    if (!entry)
    {
      std::string source = "#version 450\n#extension GL_EXT_control_flow_attributes : enable\n#define CORTAO_GLSL 1\n";
      source += extraDefines;
      source += orthographic ? "#define CORTAO_ORTHOGRAPHIC_PROJECTION 1\n" : "#define CORTAO_PERSPECTIVE_PROJECTION 1\n";
      source += "#define CORTAO_SAMPLE_COUNT " + std::to_string(sampleCount) + "\n";
      source += "#define CORTAO_RAY_COUNT " + std::to_string(CortaoRayCount) + "\n#define CORTAO_OCCLUDER_LOOKUP_TABLE 1\n";
      source += resourceText(":/shaders/cortao/CORTAOBase.h");
      std::string pass = resourceText(passHeader);
      const std::string include = "#include \"CORTAOBase.h\"";
      if (auto at = pass.find(include); at != std::string::npos) pass.erase(at, include.size());
      source += pass;
      source += resourceText(wrapper);
      entry = vsg::ShaderStage::create(VK_SHADER_STAGE_COMPUTE_BIT, "main", source);
    }
    return entry;
  }

  // An image the compute passes write and the later ones sample: both in the general layout.
  vsg::ref_ptr<vsg::Image> createComputeImage(vsg::ref_ptr<vsg::Device> device, VkFormat format, const VkExtent2D& extent, uint32_t mipLevels)
  {
    auto image = vsg::Image::create();
    image->imageType = VK_IMAGE_TYPE_2D;
    image->format = format;
    image->extent = VkExtent3D{extent.width, extent.height, 1};
    image->mipLevels = mipLevels;
    image->arrayLayers = 1;
    image->samples = VK_SAMPLE_COUNT_1_BIT;
    image->tiling = VK_IMAGE_TILING_OPTIMAL;
    image->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return image;
  }

  vsg::ref_ptr<vsg::Sampler> createNearestSampler(VkSamplerAddressMode address, float maxLod)
  {
    auto sampler = vsg::Sampler::create();
    sampler->magFilter = sampler->minFilter = VK_FILTER_NEAREST;
    sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler->addressModeU = sampler->addressModeV = sampler->addressModeW = address;
    sampler->borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    sampler->maxAnisotropy = 1.0f;
    sampler->minLod = 0.0f;
    sampler->maxLod = maxLod;
    return sampler;
  }

  // A state group that binds the compute pipeline of the shader with the descriptors, and dispatches.
  vsg::ref_ptr<vsg::Node> createComputePass(vsg::ref_ptr<vsg::ShaderStage> shaderStage, const vsg::DescriptorSetLayoutBindings& bindings, const vsg::Descriptors& descriptors, uint32_t groupsX,
                                            uint32_t groupsY)
  {
    auto setLayout = vsg::DescriptorSetLayout::create(bindings);
    auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{setLayout}, vsg::PushConstantRanges{});
    auto pipeline = vsg::ComputePipeline::create(pipelineLayout, shaderStage);
    auto descriptorSet = vsg::DescriptorSet::create(setLayout, descriptors);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(vsg::BindComputePipeline::create(pipeline));
    stateGroup->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, descriptorSet));
    stateGroup->addChild(vsg::Dispatch::create(groupsX, groupsY, 1));
    return stateGroup;
  }

  vsg::ref_ptr<vsg::PipelineBarrier> computeBarrier(VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags written, VkAccessFlags read)
  {
    return vsg::PipelineBarrier::create(from, to, 0, vsg::MemoryBarrier::create(written, read));
  }

  uint32_t groups(uint32_t pixels, uint32_t groupSize) { return (pixels + groupSize - 1) / groupSize; }
} // namespace

AmbientOcclusion::AmbientOcclusion(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Node> scene, VkExtent2D extent, double radius, vsg::vec3 backgroundTop, vsg::vec3 backgroundBottom,
                                   OcclusionAlgorithm algorithm)
    : m_Camera(camera), m_Extent(extent), m_Algorithm(algorithm), m_Radius(radius)
{
  m_SsaoParams = vsg::Value<SsaoParams>::create();
  m_SsaoParams->properties.dataVariance = vsg::DYNAMIC_DATA;
  m_SsaoParams->value().radius = static_cast<float>(radius);
  m_CompositionParams = vsg::Value<CompositionParams>::create();
  m_CompositionParams->properties.dataVariance = vsg::DYNAMIC_DATA;
  m_CompositionParams->value().background = vsg::vec4(backgroundTop, 1.0f);
  m_CompositionParams->value().backgroundBottom = vsg::vec4(backgroundBottom, 1.0f);
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
  auto gbufferGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, gbuffer);
  auto gbufferCamera = vsg::Camera::create(camera->projectionMatrix, camera->viewMatrix, vsg::ViewportState::create(extent));
  gbufferGraph->addChild(vsg::View::create(gbufferCamera, scene));

  m_Passes.push_back(gbufferGraph);

  vsg::ref_ptr<vsg::ImageInfo> occlusion, classicOcclusion;
  if (m_Algorithm != OcclusionAlgorithm::Cortao)
  {
    std::vector<vsg::ref_ptr<vsg::ImageInfo>> ssao;
    auto ssaoGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R8_UNORM}, ssao);
    ssaoGraph->addChild(vsg::View::create(createFullscreenCamera(extent),
                                          createFullscreenPass(":/shaders/ao_ssao.frag", {gbuffer[0], gbuffer[1], noiseInfo},
                                                               {vsg::DescriptorBuffer::create(kernel, 3, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                                vsg::DescriptorBuffer::create(m_SsaoParams, 4, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)})));
    m_Passes.push_back(ssaoGraph);
    classicOcclusion = occlusion = ssao[0];
  }
  if (m_Algorithm != OcclusionAlgorithm::Ssao) occlusion = createCortaoPasses(device, gbuffer);

  std::vector<vsg::ref_ptr<vsg::ImageInfo>> blurred;
  auto blurGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R8_UNORM}, blurred);
  if (m_Algorithm != OcclusionAlgorithm::Ssao)
  {
    // CORTAO's grain is blurred along the surfaces (the box blur of the SSAO smears over the edges).
    struct BlurParams
    {
      float depthScale;
      float combine = 0.0f;
      float pad1 = 0.0f, pad2 = 0.0f;
    };
    auto blurParams = vsg::Value<BlurParams>::create();
    blurParams->value().depthScale = static_cast<float>(1.0 / (0.5 * radius));
    blurParams->value().combine = m_Algorithm == OcclusionAlgorithm::Hybrid ? 1.0f : 0.0f;
    blurGraph->addChild(vsg::View::create(createFullscreenCamera(extent),
                                          createFullscreenPass(":/shaders/ao_blur_bilateral.frag", {occlusion, gbuffer[0], gbuffer[1], classicOcclusion ? classicOcclusion : occlusion},
                                                               {vsg::DescriptorBuffer::create(blurParams, 4, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)})));
  }
  else
  {
    blurGraph->addChild(vsg::View::create(createFullscreenCamera(extent), createFullscreenPass(":/shaders/ao_blur.frag", {occlusion}, {})));
  }
  m_Passes.push_back(blurGraph);

  // Read by FXAA, which looks between the pixels: filtered.
  std::vector<vsg::ref_ptr<vsg::ImageInfo>> composed;
  auto compositionGraph = createOffscreenGraph(device, extent, {VK_FORMAT_R8G8B8A8_SRGB}, composed, VK_FILTER_LINEAR);
  compositionGraph->addChild(vsg::View::create(
      createFullscreenCamera(extent), createFullscreenPass(":/shaders/ao_composite.frag", {gbuffer[0], gbuffer[2], blurred[0]},
                                                           {vsg::DescriptorBuffer::create(m_CompositionParams, 3, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)})));
  m_Passes.push_back(compositionGraph);

  m_CompositionCamera = createFullscreenCamera(extent);
  m_CompositionView = vsg::View::create(m_CompositionCamera, createFullscreenPass(":/shaders/ao_fxaa.frag", {composed[0]}, {}));
}

void AmbientOcclusion::setStrength(float strength)
{
  m_CompositionParams->value().strength = std::clamp(strength, 0.0f, 1.0f);
  m_CompositionParams->dirty();
}

void AmbientOcclusion::setTransparentBackground(bool transparent)
{
  m_CompositionParams->value().transparentBackground = transparent ? 1.0f : 0.0f;
  m_CompositionParams->dirty();
}

void AmbientOcclusion::updateProjection()
{
  m_SsaoParams->value().projection = vsg::mat4(m_Camera->projectionMatrix->transform());
  m_SsaoParams->dirty();
  if (m_CortaoParams) updateCortaoParams();
}

// CORTAO's projection parameters, as CORTAO::populateMainPassParameters makes them
// (CORTAOParameters.h; not included, as it defines functions in the header).
void AmbientOcclusion::updateCortaoParams()
{
  // VSG's matrices have the Vulkan flip of y in them (a negative [1][1]), which CORTAO
  // wants as flipY with the matrix of OpenGL: negate the row.
  const vsg::dmat4 vulkan = m_Camera->projectionMatrix->transform();
  const bool orthographic = vulkan[3][3] != 0.0;
  vsg::dmat4 projection = vulkan;
  for (int column = 0; column < 4; ++column) projection[column][1] = -projection[column][1];
  const vsg::dmat4 inverse = vsg::inverse(projection);

  const float projX = static_cast<float>(projection[0][0]), projY = static_cast<float>(projection[1][1]);
  const float projOffsetX = static_cast<float>(projection[2][0]), projOffsetY = static_cast<float>(projection[2][1]);
  const float invX = static_cast<float>(inverse[0][0]), invY = static_cast<float>(inverse[1][1]);
  const float invOffsetX = static_cast<float>(inverse[3][0]), invOffsetY = static_cast<float>(inverse[3][1]);

  constexpr float maxScreenspaceRadiusHeightRatio = 0.2f;
  constexpr float mipBias = -4.0f;
  constexpr float maxSlopeCompensation = 4.0f;
  constexpr float backfaceThreshold = 0.5f;
  const float width = static_cast<float>(m_Extent.width), height = static_cast<float>(m_Extent.height);

  auto& params = m_CortaoParams->value();
  params.packedResolution = m_Extent.width | (m_Extent.height << 16u);
  params.randomSeed = 0; // no temporal filter: a fixed pattern, not a flicker
  params.normalBias = invX / width * std::sqrt(2.0f);
  params.radius = static_cast<float>(m_Radius);
  params.unproject = vsg::vec4(+2.0f * invX, -2.0f * invY, -invX + invOffsetX, +invY + invOffsetY);
  params.projection = vsg::vec4(+0.5f * projX, -0.5f * projY, -0.5f + 0.5f * projOffsetX, -0.5f - 0.5f * projOffsetY);
  const float maxApparentCircleRadius = invY * maxScreenspaceRadiusHeightRatio * 2.0f;
  params.maxApparentCircleRadiusCoefficient = maxApparentCircleRadius / std::sqrt(maxApparentCircleRadius * maxApparentCircleRadius + 1.0f);
  params.mipBias = std::log2(projY * height * 0.5f) + mipBias;
  params.inverseMaxSlopeCompensation = 1.0f / maxSlopeCompensation;
  params.backfaceThreshold = backfaceThreshold;
  params.backfaceBoost = 0.0f;
  m_CortaoParams->dirty();

  // (All variants are compiled with the scene graph; this is the one to dispatch.)
  m_CortaoVariants->setSingleChildOn(orthographic ? 0 : 1);
}

vsg::ref_ptr<vsg::ImageInfo> AmbientOcclusion::createCortaoPasses(vsg::ref_ptr<vsg::Device> device, const std::vector<vsg::ref_ptr<vsg::ImageInfo>>& gbuffer)
{
  uint32_t sampleCount = 24;
  if (const char* env = std::getenv("SHTETL_CORTAO_SAMPLES")) sampleCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::atoi(env)), 1, 32);

  // The mipmaps of the packed data: as many as the prepass writes, and the image has.
  uint32_t mipCount = 1;
  while (mipCount < CortaoMaxMips && (std::min(m_Extent.width, m_Extent.height) >> mipCount) > 0) ++mipCount;

  // The packed data (linear depth and normal), with a view of its own for each mip to write,
  // and one with all of them to sample.
  auto packedImage = createComputeImage(device, VK_FORMAT_R32_UINT, m_Extent, mipCount);
  auto packedSampledView = vsg::createImageView(device, packedImage, VK_IMAGE_ASPECT_COLOR_BIT);
  vsg::ImageInfoList packedStorage;
  for (uint32_t mip = 0; mip < CortaoMaxMips; ++mip)
  {
    // (The prepass never writes the levels past mipCount.)
    auto view = vsg::ImageView::create(packedImage, VK_IMAGE_ASPECT_COLOR_BIT);
    view->subresourceRange.baseMipLevel = std::min(mip, mipCount - 1);
    view->subresourceRange.levelCount = 1;
    view->compile(device);
    packedStorage.push_back(vsg::ImageInfo::create(vsg::ref_ptr<vsg::Sampler>(), view, VK_IMAGE_LAYOUT_GENERAL));
  }
  auto packedSampled = vsg::ImageInfo::create(createNearestSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, static_cast<float>(mipCount)), packedSampledView, VK_IMAGE_LAYOUT_GENERAL);

  // The occlusion of the main pass: a 32 bit float, as every device can write it to a storage image.
  auto occlusionImage = createComputeImage(device, VK_FORMAT_R32_SFLOAT, m_Extent, 1);
  auto occlusionView = vsg::createImageView(device, occlusionImage, VK_IMAGE_ASPECT_COLOR_BIT);
  auto occlusionStorage = vsg::ImageInfo::create(vsg::ref_ptr<vsg::Sampler>(), occlusionView, VK_IMAGE_LAYOUT_GENERAL);
  auto occlusionSampled = vsg::ImageInfo::create(createNearestSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f), occlusionView, VK_IMAGE_LAYOUT_GENERAL);

  // The lookup table of which rays an occluder blocks.
  const QByteArray tableBytes = [] {
    QFile file(":/shaders/cortao/24x24x16x16_R16_UINT.bin");
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing resource the CORTAO occluder table");
    return file.readAll();
  }();
  auto table = vsg::ushortArray3D::create(CortaoTableWidth, CortaoTableHeight, CortaoTableDepth);
  table->properties.format = VK_FORMAT_R16_UINT;
  if (static_cast<size_t>(tableBytes.size()) != table->dataSize()) throw std::runtime_error("The CORTAO occluder table has the wrong size");
  std::memcpy(table->dataPointer(), tableBytes.constData(), table->dataSize());
  auto tableImage = vsg::Image::create(table);
  tableImage->imageType = VK_IMAGE_TYPE_3D;
  auto tableView = vsg::ImageView::create(tableImage);
  tableView->viewType = VK_IMAGE_VIEW_TYPE_3D;
  // Memory of its own, as for the other images (the data is copied when the scene graph is compiled).
  tableImage->compile(device);
  tableImage->allocateAndBindMemory(device);
  tableView->compile(device);
  auto tableInfo = vsg::ImageInfo::create(createNearestSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f), tableView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // The prepass (the same for both projections: see the shader), 8x8 pixels to a group.
  auto prepassParams = vsg::Value<CortaoPrepassParams>::create();
  prepassParams->value().packedResolution = m_Extent.width | (m_Extent.height << 16u);
  prepassParams->value().mipCount = mipCount;
  auto stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  vsg::DescriptorSetLayoutBindings prepassBindings{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                   {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stageFlags, nullptr},
                                                   {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CortaoMaxMips, stageFlags, nullptr}};
  vsg::Descriptors prepassDescriptors{vsg::DescriptorImage::create(gbuffer[0], 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                      vsg::DescriptorImage::create(gbuffer[1], 1, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                      vsg::DescriptorBuffer::create(prepassParams, 2, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                      vsg::DescriptorImage::create(packedStorage, 3, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
  auto prepass = createComputePass(cortaoShader(":/shaders/cortao/CORTAOPrepass.h", ":/shaders/ao_cortao_prepass.comp", true, sampleCount), prepassBindings, prepassDescriptors,
                                   groups(m_Extent.width, 8), groups(m_Extent.height, 8));

  // The main pass, one variant for each projection (the camera switches between them while running).
  m_CortaoParams = vsg::Value<CortaoMainParams>::create();
  m_CortaoParams->properties.dataVariance = vsg::DYNAMIC_DATA;
  vsg::DescriptorSetLayoutBindings mainBindings{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stageFlags, nullptr},
                                                {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stageFlags, nullptr}};
  vsg::Descriptors mainDescriptors{vsg::DescriptorImage::create(packedSampled, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                   vsg::DescriptorImage::create(gbuffer[1], 1, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                   vsg::DescriptorImage::create(tableInfo, 2, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                   vsg::DescriptorBuffer::create(m_CortaoParams, 3, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                   vsg::DescriptorImage::create(occlusionStorage, 4, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
  m_CortaoVariants = vsg::Switch::create();
  for (bool orthographic : {true, false})
    m_CortaoVariants->addChild(true, createComputePass(cortaoShader(":/shaders/cortao/CORTAOMainPass.h", ":/shaders/ao_cortao_main.comp", orthographic, sampleCount), mainBindings,
                                                       mainDescriptors, groups(m_Extent.width, 16), groups(m_Extent.height, 16)));
  updateCortaoParams();

  // The filter pass: the main pass' occlusion, blurred along the surfaces (13 taps), into an image of its own.
  auto filteredImage = createComputeImage(device, VK_FORMAT_R32_SFLOAT, m_Extent, 1);
  auto filteredView = vsg::createImageView(device, filteredImage, VK_IMAGE_ASPECT_COLOR_BIT);
  auto filteredStorage = vsg::ImageInfo::create(vsg::ref_ptr<vsg::Sampler>(), filteredView, VK_IMAGE_LAYOUT_GENERAL);
  auto filteredSampled = vsg::ImageInfo::create(createNearestSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f), filteredView, VK_IMAGE_LAYOUT_GENERAL);
  struct FilterParams // CORTAOFilterPassParameters
  {
    uint32_t packedResolution;
    float depthSensitivity = 512.0f;
    float normalSensitivity = 128.0f;
    uint32_t pad0 = 0;
  };
  auto filterParams = vsg::Value<FilterParams>::create();
  filterParams->value().packedResolution = m_Extent.width | (m_Extent.height << 16u);
  vsg::DescriptorSetLayoutBindings filterBindings{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                  {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stageFlags, nullptr},
                                                  {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stageFlags, nullptr},
                                                  {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stageFlags, nullptr}};
  vsg::Descriptors filterDescriptors{vsg::DescriptorImage::create(packedSampled, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     vsg::DescriptorImage::create(occlusionSampled, 1, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     vsg::DescriptorBuffer::create(filterParams, 2, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                     vsg::DescriptorImage::create(filteredStorage, 3, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
  // (A thread does 2x2 pixels, a group of 8x8 threads 16x16.)
  auto filter = createComputePass(cortaoShader(":/shaders/cortao/CORTAOFilterPass.h", ":/shaders/ao_cortao_filter.comp", true, sampleCount, "#define CORTAO_LARGE_FILTER 1\n"), filterBindings,
                                  filterDescriptors, groups(m_Extent.width, 16), groups(m_Extent.height, 16));

  // The barriers: the images are rewritten in full each frame, so their old contents may go (but
  // the reads of the last frame have to be done first); the render passes before and after only
  // order their own stages.
  const VkImageSubresourceRange packedRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
  const VkImageSubresourceRange occlusionRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  const VkAccessFlags shaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  m_Passes.push_back(vsg::PipelineBarrier::create(
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
      vsg::MemoryBarrier::create(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | shaderAccess, shaderAccess),
      vsg::ImageMemoryBarrier::create(0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, packedImage, packedRange),
      vsg::ImageMemoryBarrier::create(0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, occlusionImage,
                                      occlusionRange),
      vsg::ImageMemoryBarrier::create(0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, filteredImage,
                                      occlusionRange)));
  m_Passes.push_back(prepass);
  m_Passes.push_back(computeBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
  m_Passes.push_back(m_CortaoVariants);
  m_Passes.push_back(computeBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
  m_Passes.push_back(filter);
  m_Passes.push_back(computeBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
  return filteredSampled;
}

void AmbientOcclusion::setBackground(const vsg::vec3& top, const vsg::vec3& bottom)
{
  m_CompositionParams->value().background = vsg::vec4(top, 1.0f);
  m_CompositionParams->value().backgroundBottom = vsg::vec4(bottom, 1.0f);
  m_CompositionParams->dirty();
}

GradientBackground::GradientBackground(VkExtent2D extent, VkSampleCountFlagBits samples, const vsg::vec3& top, const vsg::vec3& bottom)
{
  m_Params = vsg::Value<Params>::create();
  m_Params->properties.dataVariance = vsg::DYNAMIC_DATA;
  setColors(top, bottom);
  m_Camera = createFullscreenCamera(extent);
  m_View = vsg::View::create(m_Camera, createFullscreenPass(":/shaders/gradient_background.frag", {},
                                                            {vsg::DescriptorBuffer::create(m_Params, 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)}, samples));
}

void GradientBackground::setColors(const vsg::vec3& top, const vsg::vec3& bottom)
{
  m_Params->value().top = vsg::vec4(top, 1.0f);
  m_Params->value().bottom = vsg::vec4(bottom, 1.0f);
  m_Params->dirty();
}

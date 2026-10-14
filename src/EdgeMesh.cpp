#include "EdgeMesh.h"

#include "ShaderResources.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
  constexpr double SharpAngleDegrees = 45.0; // as XjetStudio (EdgesBackgroundWorker.cpp)

  // A corner of a triangle, keyed by its position: the three 24-bit
  // coordinates, so that corners at the same place compare equal exactly.
  struct Corner
  {
      uint64_t lo; // x | y << 24 | (z & 0xFFFF) << 48
      uint32_t hi; // z >> 16
      uint32_t index;
      bool operator<(const Corner& o) const { return lo != o.lo ? lo < o.lo : hi < o.hi; }
      bool samePlace(const Corner& o) const { return lo == o.lo && hi == o.hi; }
  };

  struct EdgeEntry
  {
      uint64_t key; // (lower vertex << 32) | higher vertex
      uint32_t face;
      bool operator<(const EdgeEntry& o) const { return key < o.key; }
  };
} // namespace

EdgeSet computeEdges(const CompactSource& source, const std::atomic<bool>* cancel)
{
  auto checkCancel = [cancel]() {
    if (cancel && cancel->load()) throw std::runtime_error("Abort requested!");
  };
  const auto& packed = *source.vertices;
  const size_t cornerCount = packed.size();
  const size_t faceCount = cornerCount / 3;

  // Weld: sort the corners by position, and number the distinct positions.
  std::vector<uint32_t> vertexOf(cornerCount);
  std::vector<vsg::uivec3> welded;
  {
    std::vector<Corner> corners(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i)
    {
      const uint64_t x = packed[i].x & 0xFFFFFFu, y = packed[i].y & 0xFFFFFFu, z = packed[i].z & 0xFFFFFFu;
      corners[i] = {x | (y << 24) | ((z & 0xFFFFu) << 48), static_cast<uint32_t>(z >> 16), static_cast<uint32_t>(i)};
    }
    checkCancel();
    std::sort(corners.begin(), corners.end());
    checkCancel();
    for (size_t i = 0; i < cornerCount; ++i)
    {
      if (i == 0 || !corners[i].samePlace(corners[i - 1]))
      {
        const vsg::uivec3& p = packed[corners[i].index];
        welded.emplace_back(p.x & 0xFFFFFFu, p.y & 0xFFFFFFu, p.z & 0xFFFFFFu);
      }
      vertexOf[corners[i].index] = static_cast<uint32_t>(welded.size() - 1);
    }
  }

  // The unit normal of every face, from the positions in the model's own
  // units: the encoded normals are in the box space, which is scaled
  // differently along each axis.
  const vsg::dvec3 scale = source.boxRange / 16777215.0;
  std::vector<vsg::vec3> faceNormal(faceCount);
  for (size_t f = 0; f < faceCount; ++f)
  {
    vsg::dvec3 p[3];
    for (int k = 0; k < 3; ++k)
    {
      const vsg::uivec3& w = welded[vertexOf[f * 3 + static_cast<size_t>(k)]];
      p[k] = vsg::dvec3(w.x * scale.x, w.y * scale.y, w.z * scale.z);
    }
    const vsg::dvec3 n = vsg::cross(p[1] - p[0], p[2] - p[0]);
    const double len = vsg::length(n);
    faceNormal[f] = len > 0.0 && std::isfinite(len) ? vsg::vec3(float(n.x / len), float(n.y / len), float(n.z / len)) : vsg::vec3(0.0f, 0.0f, 0.0f);
  }
  checkCancel();

  // The edges, each with the faces it belongs to, sorted so that the entries
  // of one edge are together.
  std::vector<EdgeEntry> entries;
  entries.reserve(cornerCount);
  for (size_t f = 0; f < faceCount; ++f)
    for (size_t e = 0; e < 3; ++e)
    {
      uint32_t a = vertexOf[f * 3 + e], b = vertexOf[f * 3 + (e + 1) % 3];
      if (a == b) continue; // collapsed by the welding
      if (a > b) std::swap(a, b);
      entries.push_back({(static_cast<uint64_t>(a) << 32) | b, static_cast<uint32_t>(f)});
    }
  checkCancel();
  std::sort(entries.begin(), entries.end());
  checkCancel();

  const float sharpCos = static_cast<float>(std::cos(SharpAngleDegrees * 3.14159265358979323846 / 180.0));
  std::vector<uint32_t> all, sharp;
  all.reserve(entries.size()); // twice the edges at most
  for (size_t i = 0; i < entries.size();)
  {
    size_t j = i + 1;
    while (j < entries.size() && entries[j].key == entries[i].key) ++j;
    const size_t faces = j - i;

    bool isSharp = true; // a boundary, or non-manifold
    if (faces == 2)
    {
      const vsg::vec3& n0 = faceNormal[entries[i].face];
      const vsg::vec3& n1 = faceNormal[entries[i + 1].face];
      const bool degenerate = vsg::length(n0) < 0.5f || vsg::length(n1) < 0.5f;
      isSharp = degenerate || !(vsg::dot(n0, n1) >= sharpCos);
    }
    const uint32_t a = static_cast<uint32_t>(entries[i].key >> 32), b = static_cast<uint32_t>(entries[i].key & 0xFFFFFFFFu);
    all.push_back(a);
    all.push_back(b);
    if (isSharp)
    {
      sharp.push_back(a);
      sharp.push_back(b);
    }
    i = j;
  }
  checkCancel();

  auto toArray = [](const std::vector<uint32_t>& v) {
    auto array = vsg::uintArray::create(static_cast<uint32_t>(v.size()));
    std::copy(v.begin(), v.end(), array->begin());
    return array;
  };
  EdgeSet set;
  set.vertices = vsg::uivec3Array::create(static_cast<uint32_t>(welded.size()));
  std::copy(welded.begin(), welded.end(), set.vertices->begin());
  set.all = toArray(all);
  set.sharp = toArray(sharp);
  return set;
}

EdgeStyle::EdgeStyle()
{
  params = vsg::Value<EdgeParams>::create();
  params->properties.dataVariance = vsg::DYNAMIC_DATA;
  width = vsg::SetLineWidth::create(1.0f);
}

vsg::ref_ptr<vsg::Node> makeEdgesNode(const EdgeSet& edges, bool sharpOnly, const CompactSource& source, const EdgeStyle& style, bool gbuffer, bool blend)
{
  const auto& indices = sharpOnly ? edges.sharp : edges.all;
  if (!edges || !indices || indices->size() == 0) return vsg::Group::create();

  auto vertexShader = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", shaderResourceText(":/shaders/edge_lines.vert"));
  auto fragmentShader = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                                                 shaderResourceText(gbuffer ? ":/shaders/edge_lines_gbuffer.frag" : ":/shaders/edge_lines.frag"));
  auto shaderSet = vsg::ShaderSet::create(vsg::ShaderStages{vertexShader, fragmentShader});
  shaderSet->addAttributeBinding("vsg_Vertex", "", 0, VK_FORMAT_R32G32B32_UINT, vsg::vec3Array::create(1));
  shaderSet->addDescriptorBinding("lineColor", "", 1, 10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, style.params);
  shaderSet->addPushConstantRange("pc", "", VK_SHADER_STAGE_VERTEX_BIT, 0, 128);

  auto config = vsg::GraphicsPipelineConfigurator::create(shaderSet);
  config->assignDescriptor("lineColor", style.params);
  config->enableArray("vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, 12);

  auto inputAssembly = vsg::InputAssemblyState::create();
  inputAssembly->topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  config->pipelineStates.push_back(inputAssembly);

  auto rasterization = vsg::RasterizationState::create();
  rasterization->cullMode = VK_CULL_MODE_NONE;
  // As XjetStudio: a bias of a few depth units, so that the lines win against
  // the surfaces they lie on at any scale of the model (the slope factor stays
  // 0, which some drivers evaluate wrongly for lines).
  rasterization->depthBiasEnable = VK_TRUE;
  rasterization->depthBiasConstantFactor = 4.0f;
  rasterization->depthBiasSlopeFactor = 0.0f;
  config->pipelineStates.push_back(rasterization);

  // Hidden lines are removed by the depth test (reverse Z: nearer is larger),
  // and the lines don't hide anything themselves.
  auto depthStencil = vsg::DepthStencilState::create();
  depthStencil->depthTestEnable = VK_TRUE;
  depthStencil->depthWriteEnable = VK_FALSE;
  depthStencil->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
  config->pipelineStates.push_back(depthStencil);

  // The width is set while recording (see EdgeStyle).
  config->pipelineStates.push_back(vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH));

  if (gbuffer) // one blend state per target of the G-buffer
  {
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_FALSE;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    config->pipelineStates.push_back(vsg::ColorBlendState::create(std::vector<VkPipelineColorBlendAttachmentState>(3, attachment)));
  }
  else if (blend)
  {
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    config->pipelineStates.push_back(vsg::ColorBlendState::create(std::vector<VkPipelineColorBlendAttachmentState>{attachment}));
  }

  vsg::StateCommands stateCommands;
  config->init();
  config->copyTo(stateCommands);

  auto stateGroup = vsg::StateGroup::create();
  stateGroup->stateCommands.swap(stateCommands);
  stateGroup->prototypeArrayState = config->getSuitableArrayState();
  stateGroup->addChild(style.width);

  auto draw = vsg::VertexIndexDraw::create();
  draw->assignArrays(vsg::DataList{edges.vertices});
  draw->assignIndices(indices);
  draw->indexCount = indices->size();
  draw->instanceCount = 1;
  stateGroup->addChild(draw);

  auto decode = vsg::MatrixTransform::create();
  decode->matrix = vsg::translate(source.boxMin) * vsg::scale(source.boxRange);
  decode->addChild(stateGroup);
  return decode;
}

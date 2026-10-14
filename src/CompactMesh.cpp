#include "CompactMesh.h"

#include "ModelFormats.h"
#include "QtPathUtils.h"
#include "ShaderResources.h"
#ifdef SHTETL_HAS_CAD
#include "CadReader.h"
#endif

#include <vsg/animation/FindAnimations.h>
#include <vsg/io/read.h>
#include <vsg/utils/ComputeBounds.h>

#include "Logging.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

// 12 byte encoding of each vertex (as in XjetStudio)
//
//       3         2         1         0
//      10987654321098765432109876543210
//              pppppppppppppppppppppppp  24 = vertex x
//      uuuuuuuu                           8 = normal low x-coordinate
//
//              pppppppppppppppppppppppp  24 = vertex y
//      vvvvvvvv                           8 = normal low y-coordinate
//
//              pppppppppppppppppppppppp  24 = vertex z
//           uuu                           3 = high x-coordinate of the normal
//        vvv                              3 = high y-coordinate of the normal
//      bb                                 2 = corner of the triangle (0, 1, 2)

namespace
{
  vsg::usvec2 encodeOctNormal(const vsg::vec3& n)
  {
    const float invL1 = 1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) + 1e-8f);
    vsg::vec2 p(n.x * invL1, n.y * invL1);
    if (n.z < 0.0f)
    {
      const float oldX = p.x;
      p.x = (1.0f - std::fabs(p.y)) * (oldX >= 0.0f ? 1.0f : -1.0f);
      p.y = (1.0f - std::fabs(oldX)) * (p.y >= 0.0f ? 1.0f : -1.0f);
    }
    p = p * 0.5f + vsg::vec2(0.5f, 0.5f);
    return vsg::usvec2(static_cast<uint16_t>(std::round(std::clamp(p.x, 0.0f, 1.0f) * 2047.0f)),
                       static_cast<uint16_t>(std::round(std::clamp(p.y, 0.0f, 1.0f) * 2047.0f)));
  }

  std::string resourceText(const char* path)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(std::string("Missing resource ") + path);
    return file.readAll().toStdString();
  }

  // The node that draws the packed vertices: the shaders, the state, and the
  // transform that undoes the normalisation of the positions to the box.
  // `gbuffer` draws into the three targets of the ambient occlusion's G-buffer
  // instead of the window.
  vsg::ref_ptr<vsg::Node> makeNode(const CompactSource& source, bool gbuffer)
  {
    const auto& vertices = source.vertices;
    const auto& diffuse = source.diffuse;
    const bool pbr = source.pbr && source.pbrMaterial;
    auto vertexShader = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", resourceText(":/shaders/compact_mesh.vert"));
    std::string fragmentSource;
    if (pbr)
    {
      // The lighting is shared by the two passes: pasted in where they ask.
      fragmentSource = expandShaderIncludes(resourceText(gbuffer ? ":/shaders/compact_pbr_gbuffer.frag" : ":/shaders/compact_pbr.frag"));
    }
    else
      fragmentSource = resourceText(gbuffer ? ":/shaders/compact_mesh_gbuffer.frag" : ":/shaders/compact_mesh.frag");
    auto fragmentShader = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragmentSource);
    auto shaderSet = vsg::ShaderSet::create(vsg::ShaderStages{vertexShader, fragmentShader});

    // The vertex is three uints, not a vec3, and has no other attributes.
    shaderSet->addAttributeBinding("vsg_Vertex", "", 0, VK_FORMAT_R32G32B32_UINT, vsg::vec3Array::create(1));

    auto material = vsg::PhongMaterialValue::create();
    material->value().ambient = vsg::vec4{0.12f, 0.12f, 0.12f, 1.0f};
    material->value().diffuse = diffuse;
    material->value().specular = vsg::vec4{1.0f, 1.0f, 1.0f, 1.0f};
    material->value().shininess = 16.0f;
    vsg::ref_ptr<vsg::Data> materialData = pbr ? vsg::ref_ptr<vsg::Data>(source.pbrMaterial) : vsg::ref_ptr<vsg::Data>(material);
    shaderSet->addDescriptorBinding("material", "", 1, 10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, materialData);
    shaderSet->addPushConstantRange("pc", "", VK_SHADER_STAGE_VERTEX_BIT, 0, 128);

    auto config = vsg::GraphicsPipelineConfigurator::create(shaderSet);
    config->assignDescriptor("material", materialData);
    config->enableArray("vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, 12);

    // Both sides of a face are drawn (the fragment shader lights both).
    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = VK_CULL_MODE_NONE;
    config->pipelineStates.push_back(rasterization);

    if (gbuffer) // one blend state per target of the G-buffer
    {
      VkPipelineColorBlendAttachmentState attachment{};
      attachment.blendEnable = VK_FALSE;
      attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      config->pipelineStates.push_back(vsg::ColorBlendState::create(std::vector<VkPipelineColorBlendAttachmentState>(3, attachment)));
    }

    vsg::StateCommands stateCommands;
    config->init();
    config->copyTo(stateCommands);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->stateCommands.swap(stateCommands);
    stateGroup->prototypeArrayState = config->getSuitableArrayState();

    auto draw = vsg::VertexDraw::create();
    draw->assignArrays(vsg::DataList{vertices});
    draw->instanceCount = 1;
    draw->vertexCount = static_cast<uint32_t>(vertices->size());
    stateGroup->addChild(draw);

    auto decode = vsg::MatrixTransform::create();
    decode->matrix = vsg::translate(source.boxMin) * vsg::scale(source.boxRange);
    decode->addChild(stateGroup);
    return decode;
  }

  // Packs `faces` triangles, which getTriangle(i, p) delivers as three
  // vec3. Two passes over them (bounds, then encoding), so that a source that
  // reads them straight from a mapped file never needs them in memory twice.
  template <class GetTriangle>
  LoadedModel pack(size_t faces, GetTriangle getTriangle, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress)
  {
    if (faces == 0) throw std::runtime_error("The file has no triangles");
    if (faces > 0x7FFFFFFF / 3) throw std::runtime_error("Too many triangles");

    vsg::vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    // The bounds are the first half of the work, the encoding the second.
    const size_t boundsChunk = std::max<size_t>(10000, faces / 100);
    for (size_t i = 0; i < faces; ++i)
    {
      if (i % boundsChunk == 0)
      {
        if (cancel && cancel->load()) throw std::runtime_error("Abort requested!");
        if (progress) progress(static_cast<int>(i * 50 / faces));
      }
      vsg::vec3 p[3];
      getTriangle(i, p);
      for (const auto& v : p)
        for (int k = 0; k < 3; ++k)
        {
          lo[k] = std::min(lo[k], v[k]);
          hi[k] = std::max(hi[k], v[k]);
        }
    }
    if (!std::isfinite(lo.x + lo.y + lo.z + hi.x + hi.y + hi.z)) throw std::runtime_error("The mesh has non-finite coordinates");

    // A flat mesh has no extent along one axis: scale by 1 there instead of 0.
    vsg::vec3 range = hi - lo;
    for (int k = 0; k < 3; ++k)
      if (!(range[k] > 0.0f)) range[k] = 1.0f;

    auto vertices = vsg::uivec3Array::create(static_cast<uint32_t>(faces * 3));
    constexpr uint32_t maxPos = 0xFFFFFF;

    const int64_t chunk = std::max<int64_t>(10000, static_cast<int64_t>(faces) / 100);
    for (int64_t start = 0; start < static_cast<int64_t>(faces); start += chunk)
    {
      if (cancel && cancel->load()) throw std::runtime_error("Abort requested!");
      if (progress) progress(50 + static_cast<int>(start * 50 / static_cast<int64_t>(faces)));
      const int64_t end = std::min<int64_t>(start + chunk, static_cast<int64_t>(faces));

#pragma omp parallel for schedule(static)
      for (int64_t face = start; face < end; ++face)
      {
        vsg::vec3 p[3], pn[3];
        getTriangle(static_cast<size_t>(face), p);
        for (int v = 0; v < 3; ++v)
          for (int k = 0; k < 3; ++k) pn[v][k] = (p[v][k] - lo[k]) / range[k];

        // The flat face normal in the normalised box space, where it is
        // covariant: the shader's inverse transpose brings it to the view.
        const vsg::dvec3 e1(pn[1].x - pn[0].x, pn[1].y - pn[0].y, pn[1].z - pn[0].z);
        const vsg::dvec3 e2(pn[2].x - pn[0].x, pn[2].y - pn[0].y, pn[2].z - pn[0].z);
        vsg::dvec3 n = vsg::cross(e1, e2);
        const double len = vsg::length(n);
        n = len > 0.0 ? n / len : vsg::dvec3(0.0, 0.0, 1.0); // a degenerate triangle
        const vsg::usvec2 normal = encodeOctNormal(vsg::vec3(float(n.x), float(n.y), float(n.z)));

        for (uint32_t v = 0; v < 3; ++v)
        {
          uint32_t pos[3];
          for (int k = 0; k < 3; ++k)
            pos[k] = std::min(maxPos, static_cast<uint32_t>(std::round(std::clamp(pn[v][k], 0.0f, 1.0f) * static_cast<float>(maxPos))));
          vsg::uivec3 packed;
          packed.x = pos[0] | (static_cast<uint32_t>(normal.x & 0xFF) << 24);
          packed.y = pos[1] | (static_cast<uint32_t>(normal.y & 0xFF) << 24);
          packed.z = pos[2] | (static_cast<uint32_t>((normal.x >> 8) & 0x7) << 24) | (static_cast<uint32_t>((normal.y >> 8) & 0x7) << 27) | (v << 30);
          vertices->set(static_cast<size_t>(face) * 3 + v, packed);
        }
      }
    }

    LoadedModel model;
    model.compact = CompactSource{vertices, vsg::dvec3(lo.x, lo.y, lo.z), vsg::dvec3(range.x, range.y, range.z), diffuse};
    // For the physically based mode; changed live, so dynamic.
    model.compact.pbrMaterial = vsg::PbrMaterialValue::create();
    model.compact.pbrMaterial->properties.dataVariance = vsg::DYNAMIC_DATA;
    model.compact.pbrMaterial->value().baseColorFactor = diffuse;
    model.node = makeNode(model.compact, false);
    model.bounds = vsg::dbox(vsg::dvec3(lo.x, lo.y, lo.z), vsg::dvec3(hi.x, hi.y, hi.z));
    return model;
  }

  vsg::vec3 readVec3(const uchar* at)
  {
    float f[3];
    std::memcpy(f, at, sizeof f); // little endian, as STL is and as every supported host is
    return vsg::vec3(f[0], f[1], f[2]);
  }
} // namespace

vsg::ref_ptr<vsg::Node> makeCompactNode(const CompactSource& source)
{
  return makeNode(source, false);
}

vsg::ref_ptr<vsg::Node> makeCompactGBufferNode(const CompactSource& source)
{
  return makeNode(source, true);
}

LoadedModel loadStl(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open " + path.toStdString());
  const qint64 size = file.size();

  // Binary: an 80 byte header, a count and 50 bytes per triangle.
  if (size >= 84)
  {
    uchar header[84];
    if (file.read(reinterpret_cast<char*>(header), 84) != 84) throw std::runtime_error("Cannot read " + path.toStdString());
    uint32_t count;
    std::memcpy(&count, header + 80, 4);
    if (84 + 50 * static_cast<qint64>(count) == size)
    {
      // Mapped, so the triangles are read straight from the file's pages.
      QByteArray fallback;
      const uchar* data = file.map(0, size);
      if (!data)
      {
        file.seek(0);
        fallback = file.readAll();
        data = reinterpret_cast<const uchar*>(fallback.constData());
      }
      return pack(count, [data](size_t i, vsg::vec3* p) {
        const uchar* tri = data + 84 + 50 * i + 12; // after the normal
        for (int v = 0; v < 3; ++v) p[v] = readVec3(tri + 12 * v);
      }, cancel, diffuse, progress);
    }
    file.seek(0);
  }

  // ASCII: the numbers after every "vertex".
  const QByteArray text = file.readAll();
  std::vector<vsg::vec3> vertices;
  const std::string_view view(text.constData(), static_cast<size_t>(text.size()));
  const char* const end = view.data() + view.size();
  size_t at = 0;
  while ((at = view.find("vertex", at)) != std::string_view::npos)
  {
    if ((vertices.size() & 0xFFFF) == 0 && cancel && cancel->load()) throw std::runtime_error("Abort requested!");
    const char* cur = view.data() + at + 6;
    float f[3];
    for (int k = 0; k < 3; ++k)
    {
      while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n')) ++cur;
      const auto [next, ec] = std::from_chars(cur, end, f[k]);
      if (ec != std::errc()) throw std::runtime_error("Bad vertex in " + path.toStdString());
      cur = next;
    }
    vertices.emplace_back(f[0], f[1], f[2]);
    at = static_cast<size_t>(cur - view.data());
  }
  if (vertices.empty() || vertices.size() % 3 != 0) throw std::runtime_error(path.toStdString() + " is not a valid STL file");
  return pack(vertices.size() / 3, [&vertices](size_t i, vsg::vec3* p) {
    for (int v = 0; v < 3; ++v) p[v] = vertices[i * 3 + v];
  }, cancel, diffuse, progress);
}

LoadedModel load3MF(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress)
{
  const IndexedMesh mesh = read3MF(path, cancel);
  return pack(mesh.triangles.size(), [&mesh](size_t i, vsg::vec3* p) {
    for (int v = 0; v < 3; ++v) p[v] = mesh.vertices[mesh.triangles[i][v]];
  }, cancel, diffuse, progress);
}

namespace
{
  // vsgXchange reads a .glb only when the length of its binary chunk is exactly
  // the byteLength of its first buffer, but the spec has the chunk padded to a
  // multiple of 4 bytes, which exporters such as Sketchfab's do: no mesh and no
  // animation data is read then. Reads such a file with the length of the chunk
  // set to the buffer's; null when the file needs no help (or has none to give).
  vsg::ref_ptr<vsg::Node> readGlbWithPaddedBinaryChunk(const QString& path, const vsg::ref_ptr<vsg::Options>& options)
  {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    auto u32 = [&bytes](qsizetype offset) {
      uint32_t value = 0; // little endian, as the file
      std::memcpy(&value, bytes.constData() + offset, sizeof(value));
      return value;
    };
    constexpr uint32_t JsonChunk = 0x4E4F534A, BinChunk = 0x004E4942;
    if (bytes.size() < 28 || u32(0) != 0x46546C67 /* "glTF" */ || u32(16) != JsonChunk) return {};
    const qsizetype binHeader = 20 + qsizetype(u32(12));
    if (binHeader + 8 > bytes.size() || u32(binHeader + 4) != BinChunk) return {};
    const QJsonDocument json = QJsonDocument::fromJson(bytes.mid(20, qsizetype(u32(12))));
    const QJsonObject buffer = json.object().value("buffers").toArray().first().toObject();
    if (buffer.isEmpty() || buffer.contains("uri")) return {};
    const uint32_t declared = uint32_t(buffer.value("byteLength").toDouble(-1));
    const uint32_t chunkLength = u32(binHeader);
    if (declared >= chunkLength || chunkLength - declared > 3) return {};
    std::memcpy(bytes.data() + binHeader, &declared, sizeof(declared));

    auto local = vsg::Options::create(*options);
    local->extensionHint = ".glb";
    std::istringstream stream(std::string(bytes.constData(), size_t(bytes.size())));
    spdlog::info("loadModel: {} has a padded binary chunk ({} bytes for a buffer of {}); reading it with the chunk trimmed", path.toStdString(), chunkLength, declared);
    return vsg::read_cast<vsg::Node>(stream, local);
  }

  // Looks for a node that is its own ancestor. ComputeBounds and other
  // visitors recurse forever (stack overflow) on such a graph.
  bool findCycle(vsg::Object& object, std::unordered_set<const vsg::Object*>& path, std::unordered_set<const vsg::Object*>& done, std::string& where)
  {
    struct Finder : vsg::Visitor
    {
      std::unordered_set<const vsg::Object*>& path;
      std::unordered_set<const vsg::Object*>& done;
      std::string& where;
      bool found = false;
      Finder(std::unordered_set<const vsg::Object*>& p, std::unordered_set<const vsg::Object*>& d, std::string& w) : path(p), done(d), where(w) {}
      void apply(vsg::Object& o) override
      {
        if (found || done.count(&o)) return;
        if (!path.insert(&o).second)
        {
          found = true;
          where = o.className();
          return;
        }
        o.traverse(*this);
        path.erase(&o);
        done.insert(&o);
      }
    } finder(path, done, where);
    object.accept(finder);
    return finder.found;
  }
} // namespace

#ifdef SHTETL_HAS_CAD
LoadedModel loadCad(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress)
{
  // Reading and meshing the CAD file is the first 70 percent, packing the rest.
  const LoadProgress readProgress = [&progress](int p) {
    if (progress) progress(p * 70 / 100);
  };
  const LoadProgress packProgress = [&progress](int p) {
    if (progress) progress(70 + p * 30 / 100);
  };
  const IndexedMesh mesh = readCad(path, cadDeflectionFromSettings(), cancel, readProgress);
  return pack(mesh.triangles.size(), [&mesh](size_t i, vsg::vec3* p) {
    for (int v = 0; v < 3; ++v) p[v] = mesh.vertices[mesh.triangles[i][v]];
  }, cancel, diffuse, packProgress);
}
#endif

LoadedModel loadModel(const QString& path, vsg::ref_ptr<vsg::Options> options, const std::atomic<bool>* cancel, const vsg::vec4& diffuse,
                     const LoadProgress& progress)
{
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == "stl") return loadStl(path, cancel, diffuse, progress);
  if (ext == "3mf") return load3MF(path, cancel, diffuse, progress);
#ifdef SHTETL_HAS_CAD
  if (isCadExtension(ext)) return loadCad(path, cancel, diffuse, progress);
#endif

  spdlog::info("loadModel: reading {}", path.toStdString());
  spdlog::default_logger()->flush(); // the log must survive a crash
  vsg::ref_ptr<vsg::Node> node;
  if (ext == "glb") node = readGlbWithPaddedBinaryChunk(path, options);
  if (!node) node = vsg::read_cast<vsg::Node>(shtetl::pathToUtf8(shtetl::qStringToPath(path)), options);
  if (!node) throw std::runtime_error("Could not read " + path.toStdString());
  spdlog::info("loadModel: read {} ({})", path.toStdString(), node->className());

  {
    std::unordered_set<const vsg::Object*> onPath, done;
    std::string where;
    if (findCycle(*node, onPath, done, where))
    {
      spdlog::error("loadModel: {} has a cyclic scene graph (node of type {} is its own ancestor)", path.toStdString(), where);
      spdlog::default_logger()->flush();
      throw std::runtime_error("The scene graph of " + path.toStdString() + " contains a cycle");
    }
  }

  addEnvironmentLighting(node);
  spdlog::info("loadModel: environment lighting done, computing bounds");
  spdlog::default_logger()->flush();

  LoadedModel model;
  model.node = node;
  vsg::ComputeBounds computeBounds;
  computeBounds.useNodeBounds = false; // exact: the cull-node spheres are loose
  node->accept(computeBounds);
  model.bounds = computeBounds.bounds;
  vsg::FindAnimations findAnimations;
  node->accept(findAnimations);
  model.animations = findAnimations.animations;
  spdlog::info("loadModel: {} animation(s)", model.animations.size());
  return model;
}

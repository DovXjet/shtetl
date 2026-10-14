#include "ModelFormats.h"

#include "Logging.h"
#include "ShaderResources.h"

#include <vsg/all.h>
#include <vsg/io/read.h>
#include <vsg/io/VSG.h>
#include <vsgXchange/all.h>
#include <vsgXchange/models.h>
#include <vsgXchange/gltf.h>
// Newer vsgXchange no longer includes assimp.h from all.h; older versions don't ship it.
#if __has_include(<vsgXchange/assimp.h>)
#include <vsgXchange/assimp.h>
#endif

#include <QFileInfo>

#include <algorithm>
#include <set>

vsg::ref_ptr<vsg::Options> createModelOptions()
{
  auto options = vsg::Options::create();
  options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
  options->add(vsgXchange::all::create());
  // assimp leaves faces without normals unlit (a flat silhouette) unless asked to
  // generate them; this only fills in meshes that have none, e.g. OBJ without "vn".
#if __has_include(<vsgXchange/assimp.h>)
  options->setValue(vsgXchange::assimp::generate_sharp_normals, true);
#endif
  return options;
}

namespace
{
  // The PBR material values in the descriptor sets that a state group binds
  // (that is where the glTF loader puts them).
  std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> materialsBoundBy(vsg::StateGroup& stateGroup)
  {
    std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> materials;
    auto scan = [&materials](const vsg::ref_ptr<vsg::DescriptorSet>& set) {
      if (!set) return;
      for (auto& descriptor : set->descriptors)
      {
        auto buffer = descriptor.cast<vsg::DescriptorBuffer>();
        if (!buffer) continue;
        for (auto& info : buffer->bufferInfoList)
          if (auto material = info ? info->data.cast<vsg::PbrMaterialValue>() : vsg::ref_ptr<vsg::PbrMaterialValue>()) materials.push_back(material);
      }
    };
    for (auto& command : stateGroup.stateCommands)
    {
      if (auto bind = command.cast<vsg::BindDescriptorSet>()) scan(bind->descriptorSet);
      else if (auto binds = command.cast<vsg::BindDescriptorSets>())
        for (auto& set : binds->descriptorSets) scan(set);
    }
    return materials;
  }

  // Finds the PBR material values in a loaded scene.
  class FindPbrMaterials : public vsg::Visitor
  {
    public:
      std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> materials;

      // Scene graphs share nodes, and some loaded ones even contain cycles;
      // every node is visited once.
      void apply(vsg::Object& object) override
      {
        if (visited.insert(&object).second) object.traverse(*this);
      }

      void apply(vsg::StateGroup& stateGroup) override
      {
        if (!visited.insert(&stateGroup).second) return;
        for (auto& material : materialsBoundBy(stateGroup))
          if (std::find(materials.begin(), materials.end(), material) == materials.end()) materials.push_back(material);
        stateGroup.traverse(*this);
      }

    private:
      std::set<const vsg::Object*> visited;
  };
} // namespace

namespace
{
  // The source of a shader of VSG's PBR shader set. The shaders that VSG makes
  // for the common combinations of defines hold only their compiled code, so
  // the source has to come from the stock set.
  std::string stockPbrSource(VkShaderStageFlagBits stageBit)
  {
    static const std::vector<vsg::ref_ptr<vsg::ShaderStage>> stages = vsg::createPhysicsBasedRenderingShaderSet()->stages;
    for (auto& stage : stages)
      if (stage->stage == stageBit && stage->module) return stage->module->source;
    return {};
  }

  // Finds the fragment shaders of the PBR materials in a loaded scene graph and
  // adds the studio environment of pbr_environment.glsl to their lighting.
  class AddEnvironment : public vsg::Visitor
  {
    public:
      void apply(vsg::Object& object) override
      {
        if (visited.insert(&object).second) object.traverse(*this);
      }

      void apply(vsg::StateGroup& stateGroup) override
      {
        if (!visited.insert(&stateGroup).second) return;
        // The pipeline that is bound along with a PBR material is the PBR one.
        const auto materials = materialsBoundBy(stateGroup);
        if (!materials.empty())
          for (auto& command : stateGroup.stateCommands)
            if (auto bind = command.cast<vsg::BindGraphicsPipeline>())
              if (bind->pipeline)
              {
                patch(bind->pipeline->stages);
                relaxDefaultMetal(bind->pipeline->stages, materials);
              }
        stateGroup.traverse(*this);
      }

      int patched = 0;

    private:
      std::set<const vsg::Object*> visited;

      // glTF's defaults are metallic 1 and roughness 1, which many exporters
      // (the AI model generators among them) write out as they are, for a
      // material that is just a textured surface. Lit by anything but a real
      // environment a rough metal is nearly black, so, when nothing else gives
      // the metal or the roughness (no metallic-roughness texture, and not the
      // specular-glossiness workflow), such a material is made a plain dielectric.
      void relaxDefaultMetal(const vsg::ShaderStages& stages, const std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>>& materials)
      {
        for (auto& stage : stages)
          if (stage && stage->module && stage->module->hints)
            for (auto& define : stage->module->hints->defines)
              if (define == "VSG_METALLROUGHNESS_MAP" || define == "VSG_WORKFLOW_SPECGLOSS") return;
        for (auto& material : materials)
          if (material->value().metallicFactor == 1.0f && material->value().roughnessFactor == 1.0f)
          {
            material->value().metallicFactor = 0.0f;
            material->value().roughnessFactor = 0.5f;
          }
      }

      // The stages of a PBR pipeline: the fragment shader is extended; the
      // others are given their source too if they have only compiled code,
      // as the shaders of a pipeline are compiled together.
      void patch(const vsg::ShaderStages& stages)
      {
        vsg::ref_ptr<vsg::ShaderStage> fragment;
        for (auto& stage : stages)
          if (stage && stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT && stage->module) fragment = stage;
        if (!fragment) return;

        std::string source = fragment->module->source;
        if (source.empty()) source = stockPbrSource(VK_SHADER_STAGE_FRAGMENT_BIT);
        const std::string mainAnchor = "void main()";
        const std::string emissiveAnchor = "color += emissive;";
        const size_t mainAt = source.find(mainAnchor);
        const size_t emissiveAt = source.find(emissiveAnchor);
        // Not the shader expected, or done already.
        if (mainAt == std::string::npos || emissiveAt == std::string::npos || emissiveAt < mainAt || source.find("environmentLighting") != std::string::npos) return;
        // The call goes in first, being later in the text. The diffuse part is
        // raised a little (the 1.6): the textures of glTF models are albedo, and
        // look dull under the environment next to the studio lights otherwise.
        source.insert(emissiveAt, "if (pbr.specularFactor.a > 1.5) color += environmentLighting(n, v, baseColor.rgb, metallic, perceptualRoughness, ambientOcclusion * 1.6);\n    ");
        source.insert(mainAt, expandShaderIncludes("#pragma include pbr_environment\n") + "\n");
        // A new module compiles from the source, with the defines of the old one.
        fragment->module = vsg::ShaderModule::create(source, fragment->module->hints);
        for (auto& stage : stages)
          if (stage && stage != fragment && stage->module && stage->module->source.empty())
          {
            const std::string stock = stockPbrSource(stage->stage);
            if (!stock.empty()) stage->module = vsg::ShaderModule::create(stock, stage->module->hints);
          }
        ++patched;
      }
  };
} // namespace

void addEnvironmentLighting(vsg::ref_ptr<vsg::Node> node)
{
  if (!node) return;
  try
  {
    AddEnvironment visitor;
    node->accept(visitor);
    if (visitor.patched > 0) spdlog::debug("Environment lighting added to {} shader(s)", visitor.patched);
  }
  catch (const std::exception& e)
  {
    spdlog::warn("No environment lighting for this model: {}", e.what());
  }
}

std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> findPbrMaterials(vsg::Node& node)
{
  FindPbrMaterials finder;
  node.accept(finder);
  return finder.materials;
}

bool lightWithEnvironment(vsg::Node& node)
{
  const auto materials = findPbrMaterials(node);
  for (auto& material : materials) material->value().specularFactor.w = EnvironmentLightingOn;
  return !materials.empty();
}

const QStringList& modelExtensions()
{
  static const QStringList extensions = []() {
    // Readers that load 3D scenes; vsgXchange::all also holds image, font
    // and shader readers, which are not models.
    vsg::ReaderWriter::Features features;
    std::vector<vsg::ref_ptr<vsg::ReaderWriter>> readers{vsg::VSG::create(), vsgXchange::gltf::create()};
#ifdef vsgXchange_assimp
    readers.push_back(vsgXchange::assimp::create());
#endif
    for (auto& reader : readers) reader->getFeatures(features);

    // Formats that assimp lists but that are not self-contained 3D models.
    // (STEP is added back below, when the CAD reader is built.)
    static const std::set<std::string> notModels = {".txt", ".xml", ".dat", ".raw", ".json", ".bin", ".zip", ".png", ".jpg", ".jpeg",
                                                         // assimp's STEP reader only handles IFC data, not CAD STEP files.
                                                         ".stp", ".step"};

    std::set<QString> unique;
    for (auto& [ext, mask] : features.extensionFeatureMap)
    {
      if (!(mask & vsg::ReaderWriter::READ_FILENAME)) continue;
      std::string e = ext.string();
      std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
      if (e.empty() || notModels.count(e)) continue;
      unique.insert(QString::fromStdString(e));
    }
#ifdef SHTETL_HAS_CAD
    // Read by loadModel() with OpenCASCADE, not through vsg.
    for (const char* ext : {".step", ".stp", ".iges", ".igs"}) unique.insert(ext);
#endif
    return QStringList(unique.begin(), unique.end());
  }();
  return extensions;
}

QStringList modelNameFilters()
{
  QStringList filters;
  for (const QString& ext : modelExtensions()) filters << "*" + ext;
  return filters;
}

bool isModelFile(const QString& path)
{
  return modelExtensions().contains("." + QFileInfo(path).suffix().toLower());
}

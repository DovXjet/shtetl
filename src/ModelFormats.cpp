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
  return options;
}

namespace
{
  // Finds the fragment shaders of VSG's PBR shader in a loaded scene graph and
  // adds the studio environment of pbr_environment.glsl to their lighting.
  class AddEnvironment : public vsg::Visitor
  {
    public:
      void apply(vsg::Object& object) override { object.traverse(*this); }

      void apply(vsg::StateGroup& stateGroup) override
      {
        for (auto& command : stateGroup.stateCommands)
          if (auto bind = command.cast<vsg::BindGraphicsPipeline>())
            if (bind->pipeline)
              for (auto& stage : bind->pipeline->stages) patch(stage);
        stateGroup.traverse(*this);
      }

      int patched = 0;

    private:
      void patch(const vsg::ref_ptr<vsg::ShaderStage>& stage)
      {
        if (!stage || stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT || !stage->module) return;
        std::string source = stage->module->source;
        const std::string mainAnchor = "void main()";
        const std::string emissiveAnchor = "color += emissive;";
        const size_t mainAt = source.find(mainAnchor);
        const size_t emissiveAt = source.find(emissiveAnchor);
        // Another shader (Phong, flat, ...), or one that is done already.
        if (mainAt == std::string::npos || emissiveAt == std::string::npos || emissiveAt < mainAt || source.find("environmentLighting") != std::string::npos) return;
        if (source.find("PbrMaterial") == std::string::npos) return;
        // The call goes in first, being later in the text.
        source.insert(emissiveAt, "if (pbr.specularFactor.a > 1.5) color += environmentLighting(n, v, baseColor.rgb, metallic, perceptualRoughness, ambientOcclusion);\n    ");
        source.insert(mainAt, expandShaderIncludes("#pragma include pbr_environment\n") + "\n");
        stage->module = vsg::ShaderModule::create(source, stage->module->hints);
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

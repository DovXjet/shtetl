#include "ModelFormats.h"

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

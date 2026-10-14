// A headless thumbnail: `thumbtool model.glb out.png` renders a model with the
// code of the thumbnailer (a device of its own, no window), for looking at
// changes to the thumbnails without running shtetl. Not installed.

#include "CompactMesh.h"
#include "ModelFormats.h"
#include "ThumbnailRenderer.h"

#include <vsg/all.h>

#include <QFileInfo>
#include <QImage>

#include <cmath>
#include <iostream>

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::cerr << "usage: thumbtool model out.png [--no-environment] [--pbr metallic roughness]\n";
    return 2;
  }
  const QString path = QString::fromLocal8Bit(argv[1]);
  bool environment = true, pbr = false;
  float metallic = 0.0f, roughness = 0.5f;
  for (int i = 3; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--no-environment") environment = false;
    else if (arg == "--pbr" && i + 2 < argc)
    {
      pbr = true; // the PBR shader for STL and 3MF models, as in the viewer
      metallic = std::stof(argv[++i]);
      roughness = std::stof(argv[++i]);
    }
  }

  try
  {
    auto instance = vsg::Instance::create(vsg::Names{}, vsg::Names{});
    auto [physicalDevice, queueFamily] = instance->getPhysicalDeviceAndQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    if (!physicalDevice) throw std::runtime_error("No Vulkan device");
    vsg::QueueSettings queueSettings{vsg::QueueSetting{queueFamily, {1.0}}};
    auto device = vsg::Device::create(physicalDevice, queueSettings, vsg::Names{}, vsg::Names{});

    ThumbnailRenderer renderer;
    renderer.setDevice(device);
    renderer.setExtent({256, 256});

    // The colors of the thumbnailer (ThumbnailService.cpp), sRGB made linear.
    auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    auto rgb = [&](float r, float g, float b) { return vsg::vec4(linear(r), linear(g), linear(b), 1.0f); };
    const QString ext = QFileInfo(path).suffix().toLower();
    vsg::vec4 color = rgb(0.93f, 0.93f, 0.93f);
    if (ext == "stl") color = rgb(1.0f, 0.8f, 0.8f);
    if (ext == "3mf") color = rgb(0.8f, 1.0f, 0.8f);

    LoadedModel model = loadModel(path, createModelOptions(), nullptr, color);
    if (pbr && model.compact)
    {
      model.compact.pbr = true;
      model.compact.pbrMaterial->value().metallicFactor = metallic;
      model.compact.pbrMaterial->value().roughnessFactor = roughness;
      model.node = makeCompactNode(model.compact);
    }
    const bool environmentLit = environment && (lightWithEnvironment(*model.node) || pbr);
    auto data = renderer.render(model.node, model.bounds, environmentLit);
    if (!data) throw std::runtime_error("The render failed");
    if (!thumbnailImage(*data).save(QString::fromLocal8Bit(argv[2]), "PNG")) throw std::runtime_error("Could not save the image");
    std::cout << (environmentLit ? "environment lit" : "studio lit") << "\n";
  }
  catch (const std::exception& e)
  {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  catch (const vsg::Exception& e)
  {
    std::cerr << "error: " << e.message << " (VkResult " << e.result << ")\n";
    return 1;
  }
  return 0;
}

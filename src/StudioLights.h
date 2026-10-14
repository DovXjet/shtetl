#pragma once

#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/nodes/AbsoluteTransform.h>

// Soft lighting, fixed to the camera as Blender's studio lights are in its
// solid mode: a good deal of ambient light, a key light from the upper left
// and a weaker fill from the right. VSG's own headlight is all on the view
// axis with almost no ambient light, which leaves every face that doesn't
// look straight at the camera very dark. Used by the viewport and the thumbnails.
inline vsg::ref_ptr<vsg::Node> createStudioLights()
{
  auto lights = vsg::AbsoluteTransform::create();

  auto ambient = vsg::AmbientLight::create();
  ambient->name = "ambient";
  ambient->color.set(1.0f, 1.0f, 1.0f);
  ambient->intensity = 0.25f;
  lights->addChild(ambient);

  // The direction is the one the light travels in, in eye space (x right,
  // y up, looking down -z).
  auto addDirectional = [&](const char* name, float intensity, const vsg::dvec3& direction) {
    auto light = vsg::DirectionalLight::create();
    light->name = name;
    light->color.set(1.0f, 1.0f, 1.0f);
    light->intensity = intensity;
    light->direction = vsg::normalize(direction);
    lights->addChild(light);
  };
  addDirectional("key", 0.55f, vsg::dvec3(0.5, -0.6, -1.0));
  addDirectional("fill", 0.25f, vsg::dvec3(-0.7, 0.2, -1.0));
  return lights;
}

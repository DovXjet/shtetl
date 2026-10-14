#pragma once

#include <vsg/io/Options.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/material.h>

#include <QString>
#include <QStringList>

#include <vector>

// The options used to read models: vsgXchange's readers (assimp, glTF, ...)
// on top of the VSG native formats. Shared by the viewer and the thumbnailer.
vsg::ref_ptr<vsg::Options> createModelOptions();

// Adds the studio environment (pbr_environment.glsl) to the lighting of the PBR
// materials (glTF) in a loaded scene graph, before it is compiled. A material
// takes part when the alpha of its specularFactor, which VSG's PBR shader does
// not otherwise use, is 2 (see Viewport3D); the others render as they did.
void addEnvironmentLighting(vsg::ref_ptr<vsg::Node> node);

// The PBR material values (the glTF materials) of a scene graph.
std::vector<vsg::ref_ptr<vsg::PbrMaterialValue>> findPbrMaterials(vsg::Node& node);

// The value of the alpha of a PBR material's specularFactor that asks for the environment lighting.
constexpr float EnvironmentLightingOn = 2.0f;

// Has all the PBR materials of a scene graph lit by the environment (see
// addEnvironmentLighting). Returns whether there are any: their lighting then
// is the environment's, and the scene's studio lights should be off.
bool lightWithEnvironment(vsg::Node& node);

// Lower-case extensions, with the leading dot (".stl"), of the 3D model files
// that createModelOptions() can read. Built from the readers that are
// registered, so it follows what vsgXchange was built with.
const QStringList& modelExtensions();

// Name filters for QDir/QFileDialog ("*.stl", ...).
QStringList modelNameFilters();

bool isModelFile(const QString& path);

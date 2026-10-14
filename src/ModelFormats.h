#pragma once

#include <vsg/io/Options.h>
#include <vsg/nodes/Node.h>

#include <QString>
#include <QStringList>

// The options used to read models: vsgXchange's readers (assimp, glTF, ...)
// on top of the VSG native formats. Shared by the viewer and the thumbnailer.
vsg::ref_ptr<vsg::Options> createModelOptions();

// Adds the studio environment (pbr_environment.glsl) to the lighting of the PBR
// materials (glTF) in a loaded scene graph, before it is compiled. A material
// takes part when the alpha of its specularFactor, which VSG's PBR shader does
// not otherwise use, is 2 (see Viewport3D); the others render as they did.
void addEnvironmentLighting(vsg::ref_ptr<vsg::Node> node);

// Lower-case extensions, with the leading dot (".stl"), of the 3D model files
// that createModelOptions() can read. Built from the readers that are
// registered, so it follows what vsgXchange was built with.
const QStringList& modelExtensions();

// Name filters for QDir/QFileDialog ("*.stl", ...).
QStringList modelNameFilters();

bool isModelFile(const QString& path);

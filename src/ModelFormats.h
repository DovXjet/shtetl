#pragma once

#include <vsg/io/Options.h>

#include <QString>
#include <QStringList>

// The options used to read models: vsgXchange's readers (assimp, glTF, ...)
// on top of the VSG native formats. Shared by the viewer and the thumbnailer.
vsg::ref_ptr<vsg::Options> createModelOptions();

// Lower-case extensions, with the leading dot (".stl"), of the 3D model files
// that createModelOptions() can read. Built from the readers that are
// registered, so it follows what vsgXchange was built with.
const QStringList& modelExtensions();

// Name filters for QDir/QFileDialog ("*.stl", ...).
QStringList modelNameFilters();

bool isModelFile(const QString& path);

#pragma once

#include <vsg/all.h>

#include <QString>

#include <atomic>
#include <vector>

// Triangle meshes in XjetStudio's compact form (ported from its
// CreateCompactMeshNode): per triangle three 12-byte vertices - 24-bit
// positions normalised to the bounding box and the flat face normal in 11-bit
// octahedral encoding - drawn by a shader that decodes them. No index buffer,
// and no CPU-side copy of the mesh after it is packed.

// A model ready to add to a scene, with its bounds in its own coordinates
// (vsg::ComputeBounds doesn't understand the packed vertices).
struct LoadedModel
{
    vsg::ref_ptr<vsg::Node> node;
    vsg::dbox bounds;
};

// Loads a model file: STL and 3MF as compact meshes, anything else through
// vsg::read (vsgXchange). Throws std::runtime_error on failure, and, when
// *cancel turns true while an STL/3MF is being packed, "Abort requested!".
// `diffuse` is the color of the compact meshes (STL and 3MF); the default is a
// neutral light grey.
LoadedModel loadModel(const QString& path, vsg::ref_ptr<vsg::Options> options, const std::atomic<bool>* cancel = nullptr,
                      const vsg::vec4& diffuse = vsg::vec4(0.85f, 0.85f, 0.85f, 1.0f));

// An indexed triangle mesh, as 3MF delivers it.
struct IndexedMesh
{
    std::vector<vsg::vec3> vertices;
    std::vector<vsg::uivec3> triangles;
};

// Reads all the build items of a 3MF file (with their transforms and
// components) into one mesh in millimeters. Throws std::runtime_error.
IndexedMesh read3MF(const QString& path);

// STL, binary or ASCII, into a mesh of unconnected triangles.
LoadedModel loadStl(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse);
LoadedModel load3MF(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse);

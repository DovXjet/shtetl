#pragma once

#include <vsg/animation/Animation.h>
#include <vsg/all.h>

#include <QString>

#include <atomic>
#include <functional>
#include <vector>

// Triangle meshes in XjetStudio's compact form (ported from its
// CreateCompactMeshNode): per triangle three 12-byte vertices - 24-bit
// positions normalised to the bounding box and the flat face normal in 11-bit
// octahedral encoding - drawn by a shader that decodes them. No index buffer,
// and no CPU-side copy of the mesh after it is packed.

// What it takes to draw a compact mesh with another pipeline (the G-buffer
// pass of the ambient occlusion). Empty (no vertices) for other formats.
struct CompactSource
{
    vsg::ref_ptr<vsg::uivec3Array> vertices;
    vsg::dvec3 boxMin, boxRange;
    vsg::vec4 diffuse;
    // Drawn with the physically based shaders instead of the Phong ones; the
    // material is shared by the nodes made from this source, and may be changed
    // (then dirty()) while they are shown.
    bool pbr = false;
    vsg::ref_ptr<vsg::PbrMaterialValue> pbrMaterial;
    // Per vertex, the sRGB color of its triangle as 0xAABBGGRR with AA = 255
    // (0: the material's color). Null when the model has no colors of its own.
    vsg::ref_ptr<vsg::uintArray> colors;
    // Alpha blended (diffuse.a is the opacity) and without a depth write, for
    // the see-through parts; not for the G-buffer pass.
    bool blend = false;
    explicit operator bool() const { return vertices.valid(); }
};

// How a part of a model is shown: not at all, as a see-through ghost, or solid.
enum class PartState : char
{
    Off,
    Transparent,
    Opaque
};

// One node of the hierarchy of a 3MF or STEP file (objects and components,
// assemblies and parts), flat in depth first order: a parent comes before its
// children. A leaf owns the triangles [firstTriangle, endTriangle) of the mesh
// (which are the vertices 3 * first .. 3 * end of the compact array); a group
// owns none of its own.
struct PartNode
{
    QString name;
    int parent = -1; // index of the parent node, -1 for a root
    bool leaf = true;
    uint32_t firstTriangle = 0, endTriangle = 0;
};

// A model ready to add to a scene, with its bounds in its own coordinates
// (vsg::ComputeBounds doesn't understand the packed vertices).
struct LoadedModel
{
    vsg::ref_ptr<vsg::Node> node;
    vsg::dbox bounds;
    CompactSource compact;
    std::vector<PartNode> parts; // of the compact mesh; empty when the file has no hierarchy
    vsg::Animations animations; // of the file (glTF, ...), none for STL and 3MF
};

// The node that draws a compact mesh into the window (with source.pbr, the
// physically based shaders are used).
vsg::ref_ptr<vsg::Node> makeCompactNode(const CompactSource& source);

// The node that draws a compact mesh into the three targets of the G-buffer
// (see resources/shaders/compact_mesh_gbuffer.frag).
vsg::ref_ptr<vsg::Node> makeCompactGBufferNode(const CompactSource& source);

// Called while an STL/3MF is packed, from the loading thread, with 0..100.
using LoadProgress = std::function<void(int percent)>;

// Loads a model file: STL and 3MF as compact meshes, anything else through
// vsg::read (vsgXchange). Throws std::runtime_error on failure, and, when
// *cancel turns true while an STL/3MF is being packed, "Abort requested!".
// `diffuse` is the color of the compact meshes (STL and 3MF); the default is a
// neutral light grey.
LoadedModel loadModel(const QString& path, vsg::ref_ptr<vsg::Options> options, const std::atomic<bool>* cancel = nullptr,
                      const vsg::vec4& diffuse = vsg::vec4(0.85f, 0.85f, 0.85f, 1.0f), const LoadProgress& progress = {});

// Drops the nodes of a hierarchy that have no triangles, and the whole
// hierarchy if it has fewer than two parts.
void pruneParts(std::vector<PartNode>& parts);

// An indexed triangle mesh, as 3MF delivers it.
struct IndexedMesh
{
    std::vector<vsg::vec3> vertices;
    std::vector<vsg::uivec3> triangles;
    // Per triangle, as CompactSource::colors; empty when there are no colors.
    std::vector<uint32_t> triangleColors;
    // The hierarchy of the file, when it has one (see PartNode).
    std::vector<PartNode> parts;
};

// Reads all the build items of a 3MF file (with their transforms and
// components) into one mesh in millimeters. Throws std::runtime_error.
IndexedMesh read3MF(const QString& path, const std::atomic<bool>* cancel = nullptr);

// STL, binary or ASCII, into a mesh of unconnected triangles.
LoadedModel loadStl(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress = {});
LoadedModel load3MF(const QString& path, const std::atomic<bool>* cancel, const vsg::vec4& diffuse, const LoadProgress& progress = {});

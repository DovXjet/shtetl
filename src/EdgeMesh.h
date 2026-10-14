#pragma once

#include "CompactMesh.h"

#include <vsg/all.h>

#include <atomic>

// The edges of a compact mesh, computed as XjetStudio does (merge_vertices +
// compute_sharp_edges): the vertices are welded, and an edge is sharp when it
// has one face (a boundary), more than two (non-manifold) or a degenerate
// face next to it, or when the angle between its two faces exceeds 45 degrees.

// The welded vertices (the positions only, packed as in the compact mesh so
// that the same decoding applies) and the edges as pairs of indices into them:
// every edge of the mesh once, and the sharp ones among them.
struct EdgeSet
{
    vsg::ref_ptr<vsg::uivec3Array> vertices;
    vsg::ref_ptr<vsg::uintArray> all, sharp;
    explicit operator bool() const { return vertices.valid(); }
};

// Throws std::runtime_error("Abort requested!") when *cancel turns true.
EdgeSet computeEdges(const CompactSource& source, const std::atomic<bool>* cancel = nullptr);

// The uniform of the edge shaders.
struct EdgeParams
{
    vsg::vec4 color{0.0f, 0.0f, 0.0f, 1.0f}; // linear rgb
    // The depth fade of the wireframe: lines at the view depth x or less are
    // opaque, those at y or more have the alpha z, in between it is linear.
    // z = 1 is no fade.
    vsg::vec4 fade{0.0f, 1.0f, 1.0f, 0.0f};
};

// What the edge lines are drawn with; changed while they are shown.
struct EdgeStyle
{
    vsg::ref_ptr<vsg::Value<EdgeParams>> params;
    vsg::ref_ptr<vsg::SetLineWidth> width;
    EdgeStyle();
};

// The node that draws the edges (the sharp ones, or all of them) over the mesh
// `source`: into the window, or with `gbuffer` into the targets of the ambient
// occlusion's G-buffer. With `blend` (the window only) the lines are blended
// by the alpha that the fade gives them. An empty group when there are none.
vsg::ref_ptr<vsg::Node> makeEdgesNode(const EdgeSet& edges, bool sharpOnly, const CompactSource& source, const EdgeStyle& style, bool gbuffer, bool blend = false);

#version 450
#extension GL_ARB_separate_shader_objects : enable

// The edge lines in the G-buffer pass of the ambient occlusion mode; the
// targets are those of compact_mesh_gbuffer.frag.

layout(set = 1, binding = 10) uniform LineParams
{
    vec4 color;
    vec4 fade; // view depths: opaque up to x, alpha z from y on (see EdgeParams)
} line;

layout(location = 0) in vec3 frag_vertex;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outColor;

void main()
{
    // w is the view space z, negative: geometry, not background.
    outPosition = vec4(frag_vertex, frag_vertex.z);
    // A normal towards the viewer; a line has none of its own.
    outNormal = vec4(0.5, 0.5, 1.0, 1.0);
    outColor = vec4(line.color.rgb, 1.0);
}

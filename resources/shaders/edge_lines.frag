#version 450
#extension GL_ARB_separate_shader_objects : enable

// The color of the edge lines (linear), fading with the depth in the wireframe.
layout(set = 1, binding = 10) uniform LineParams
{
    vec4 color;
    vec4 fade; // view depths: opaque up to x, alpha z from y on (see EdgeParams)
} line;

layout(location = 0) in vec3 frag_vertex;

layout(location = 0) out vec4 outColor;

void main()
{
    float depth = -frag_vertex.z;
    float t = clamp((depth - line.fade.x) / max(line.fade.y - line.fade.x, 1e-6), 0.0, 1.0);
    outColor = vec4(line.color.rgb, mix(1.0, line.fade.z, t));
}

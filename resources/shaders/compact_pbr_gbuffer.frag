#version 450
#extension GL_ARB_separate_shader_objects : enable

// The G-buffer pass of the ambient occlusion mode for the compact meshes with
// the physically based lighting; the targets are those of
// compact_mesh_gbuffer.frag.

#pragma include_common

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outColor;

void main()
{
    vec3 n = normalize(normal);
    if (!gl_FrontFacing) n = -n;
    outPosition = vec4(pos, pos.z);
    outNormal = vec4(n * 0.5 + 0.5, 1.0);
    outColor = vec4(shade(n, pos), 1.0);
}

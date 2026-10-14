#version 450
#extension GL_ARB_separate_shader_objects : enable

// The compact meshes with the physically based lighting (see
// compact_pbr_common.glsl, which is pasted in below), faces lit from both sides.

#pragma include_common

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 n = normalize(normal);
    if (!gl_FrontFacing) n = -n;
    outColor = vec4(shade(n, pos), material.baseColorFactor.a);
}

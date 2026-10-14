// The physically based lighting of the compact meshes (the "Use PBR shader"
// mode), shared by compact_pbr.frag and compact_pbr_gbuffer.frag; the
// environment is in pbr_environment.glsl.

layout(set = 1, binding = 10) uniform MaterialData
{
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    vec4 diffuseFactor;
    vec4 specularFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaMask;
    float alphaMaskCutoff;
} material;

layout(location = 0) in vec3 normal;
layout(location = 1) in vec3 pos;

#pragma include pbr_environment

// The color of the surface at `pos` (view space) with the normal `n`.
vec3 shade(vec3 n, vec3 pos)
{
    return environmentLighting(n, -normalize(pos), material.baseColorFactor.rgb, material.metallicFactor, material.roughnessFactor, 1.0) + material.emissiveFactor.rgb;
}

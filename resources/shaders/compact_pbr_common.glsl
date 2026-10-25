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
layout(location = 2) flat in uint vcolor;

// The color a triangle's file gave it (sRGB bytes, 0xAABBGGRR; 0 for none),
// in linear light, or `fallback`.
vec3 fileColor(uint packed, vec3 fallback)
{
    if ((packed >> 24) == 0u) return fallback;
    vec3 c = vec3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu) / 255.0;
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

#pragma include pbr_environment

// The color of the surface at `pos` (view space) with the normal `n`.
vec3 shade(vec3 n, vec3 pos)
{
    return environmentLighting(n, -normalize(pos), fileColor(vcolor, material.baseColorFactor.rgb), material.metallicFactor, material.roughnessFactor, 1.0) + material.emissiveFactor.rgb;
}

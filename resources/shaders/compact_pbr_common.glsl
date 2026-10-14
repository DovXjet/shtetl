// The physically based lighting of the compact meshes (the "Use PBR shader"
// mode), shared by compact_pbr.frag and compact_pbr_gbuffer.frag: GGX with an
// analytic studio environment instead of a cube map. The environment - a sky
// that fades into a dark floor, two soft boxes and a rim strip - is fixed to
// the camera, as the studio lights of the Phong mode are.

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

const float PI = 3.14159265;

// A soft box: a lobe around `center`, that is wider and dimmer (with the same
// energy) the rougher the reflection that looks at it.
float softBox(vec3 dir, vec3 center, float sharpness, float roughness)
{
    float e = mix(sharpness, 1.5, roughness);
    return pow(max(dot(dir, center), 0.0), e) * (sharpness + 1.0) / (e + 1.0);
}

// Radiance of the environment seen along `dir` (view space: y up, the camera
// looks down -z), blurred by the roughness.
vec3 environment(vec3 dir, float roughness)
{
    float up = dir.y;
    vec3 sky = mix(vec3(0.75, 0.82, 0.95), vec3(0.45, 0.50, 0.58), 1.0 - smoothstep(0.0, 1.0, up));
    vec3 floorColor = vec3(0.10, 0.09, 0.08);
    vec3 color = mix(floorColor, sky, smoothstep(-0.25 - roughness * 0.5, 0.25 + roughness * 0.5, up));
    // The key box (upper left, in front), the fill (right) and a rim strip behind.
    color += vec3(1.00, 0.97, 0.92) * 7.0 * softBox(dir, normalize(vec3(-0.5, 0.6, 0.6)), 30.0, roughness);
    color += vec3(0.85, 0.92, 1.00) * 3.0 * softBox(dir, normalize(vec3(0.8, 0.1, 0.4)), 20.0, roughness);
    color += vec3(1.00, 1.00, 1.00) * 4.0 * softBox(dir, normalize(vec3(0.0, 0.5, -0.9)), 25.0, roughness);
    return color;
}

// The light that reaches a diffuse surface facing `n`.
vec3 irradiance(vec3 n)
{
    vec3 color = mix(vec3(0.07, 0.065, 0.06), vec3(0.34, 0.37, 0.43), n.y * 0.5 + 0.5);
    color += vec3(1.00, 0.97, 0.92) * 0.50 * max(dot(n, normalize(vec3(-0.5, 0.6, 0.6))), 0.0);
    color += vec3(0.85, 0.92, 1.00) * 0.20 * max(dot(n, normalize(vec3(0.8, 0.1, 0.4))), 0.0);
    return color;
}

// Karis' analytic fit of the split-sum environment BRDF.
vec3 environmentBrdf(vec3 f0, float roughness, float nDotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// The color of the surface at `pos` (view space) with the normal `n`.
vec3 shade(vec3 n, vec3 pos)
{
    vec3 v = -normalize(pos);
    float nDotV = max(dot(n, v), 1e-3);
    float metallic = clamp(material.metallicFactor, 0.0, 1.0);
    float roughness = clamp(material.roughnessFactor, 0.04, 1.0);
    vec3 base = material.baseColorFactor.rgb;

    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 specular = environment(reflect(-v, n), roughness) * environmentBrdf(f0, roughness, nDotV);
    vec3 diffuse = base * (1.0 - metallic) * irradiance(n) * (1.0 - environmentBrdf(vec3(0.04), roughness, nDotV));
    return diffuse + specular + material.emissiveFactor.rgb;
}

#version 450

// A blur of the occlusion that stays on its surface: neighbours count by how
// close they are in depth and how much their normals agree, so the grain of
// CORTAO is smoothed without smearing over the edges.

layout(binding = 0) uniform sampler2D samplerAO;
layout(binding = 1) uniform sampler2D samplerPositionDepth;
layout(binding = 2) uniform sampler2D samplerNormal;
layout(binding = 3) uniform sampler2D samplerAO2; // the other occlusion of the hybrid
layout(binding = 4) uniform UBO
{
    float depthScale; // 1 / the depth difference that still counts as the same surface
    float combine;    // 1: the darker of the two occlusions
    float pad1, pad2;
} ubo;

float occlusionAt(vec2 uv)
{
    float ao = texture(samplerAO, uv).r;
    if (ubo.combine > 0.5) ao = min(ao, texture(samplerAO2, uv).r);
    return ao;
}

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outFragColor;

void main()
{
    const int range = 3;
    const vec2 texel = 1.0 / vec2(textureSize(samplerAO, 0));
    const float centerZ = texture(samplerPositionDepth, inUV).a;
    const vec4 centerNormalTexel = texture(samplerNormal, inUV);
    if (centerZ >= 0.0 || centerNormalTexel.a < 0.5)
    {
        outFragColor = 1.0;
        return;
    }
    const vec3 centerNormal = normalize(centerNormalTexel.rgb * 2.0 - 1.0);

    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = -range; y <= range; y++)
    {
        for (int x = -range; x <= range; x++)
        {
            const vec2 uv = inUV + vec2(float(x), float(y)) * texel;
            const float z = texture(samplerPositionDepth, uv).a;
            const vec4 normalTexel = texture(samplerNormal, uv);
            if (z >= 0.0 || normalTexel.a < 0.5) continue; // the sky says nothing (and may hold garbage)

            const float dz = (z - centerZ) * ubo.depthScale;
            const float normalDot = max(dot(normalize(normalTexel.rgb * 2.0 - 1.0), centerNormal), 0.0);
            const float spatial = exp(-float(x * x + y * y) / 8.0);
            const float weight = spatial * exp(-dz * dz) * pow(normalDot, 12.0);
            sum += occlusionAt(uv) * weight;
            weightSum += weight;
        }
    }
    outFragColor = weightSum > 0.0 ? sum / weightSum : occlusionAt(inUV);
}

#version 450

// Screen space ambient occlusion, from XjetStudio's ssao/ssao-ortho.frag
// (works for both projections: the samples are projected with the camera's
// matrix). The radius is in view space units, and the depth bias is a
// fraction of it, as models range from a millimeter to a meter.

layout(binding = 0) uniform sampler2D samplerPositionDepth;
layout(binding = 1) uniform sampler2D samplerNormal;
layout(binding = 2) uniform sampler2D ssaoNoise;

layout(constant_id = 0) const int SSAO_KERNEL_SIZE = 64;

layout(binding = 3) uniform UBOSSAOKernel
{
    vec4 samples[SSAO_KERNEL_SIZE];
} uboSSAOKernel;

layout(binding = 4) uniform UBO
{
    mat4 projection;
    float radius;
    float biasFraction;
    float pad0;
    float pad1;
} ubo;

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outFragColor;

void main()
{
    vec4 posDepth = texture(samplerPositionDepth, inUV);
    vec3 fragPos = posDepth.rgb;
    vec4 normalTexel = texture(samplerNormal, inUV);

    // The background: no geometry, no occlusion.
    if (normalTexel.a < 0.5 || posDepth.a >= 0.0) { outFragColor = 1.0; return; }
    vec3 normal = normalize(normalTexel.rgb * 2.0 - 1.0);

    // A random rotation of the kernel around the normal, from a tiled noise.
    ivec2 texDim = textureSize(samplerPositionDepth, 0);
    ivec2 noiseDim = textureSize(ssaoNoise, 0);
    vec2 noiseUV = vec2(float(texDim.x) / float(noiseDim.x), float(texDim.y) / float(noiseDim.y)) * inUV;
    vec3 randomVec = texture(ssaoNoise, noiseUV).xyz;

    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(tangent, normal);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float radius = ubo.radius;
    float bias = radius * ubo.biasFraction;

    float occlusion = 0.0;
    int validSamples = 0;
    for (int i = 0; i < SSAO_KERNEL_SIZE; i++)
    {
        vec3 samplePos = fragPos + (TBN * uboSSAOKernel.samples[i].xyz) * radius;

        vec4 offset = ubo.projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        // View space z of the surface seen at the sample's pixel (negative on
        // geometry; the background is 0 and says nothing).
        float sampleDepth = texture(samplerPositionDepth, offset.xy).a;
        if (sampleDepth >= 0.0) continue;
        validSamples++;

        // Only surfaces within the radius of this point count.
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    outFragColor = validSamples > 0 ? 1.0 - occlusion / float(validSamples) : 1.0;
}

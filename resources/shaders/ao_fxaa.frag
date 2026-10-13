#version 450

// Post-process FXAA pass (from XjetStudio's ssao/fxaa.frag) that runs after
// the composition of the ambient occlusion mode, which has no multisampling of
// its own. Reads the composition output as a 1x sampled texture and writes the
// antialiased result to the swapchain.
//
// Algorithm: classic FXAA "console" variant — one luma-based contrast
// check, then a 4-corner direction estimate, then two pairs of taps
// along the detected edge. ~50 lines, no UBO, no precomputed LUTs.
// Self-derives the per-pixel step from textureSize(samplerColor, 0)
// so it works at any extent without rebinding constants on resize.
//
// Conservative defaults: rejects pixels whose local luma range is
// below max(0.0312, 0.125 * lumaMax) to avoid blurring smooth gradients.
// SPAN_MAX = 8 caps the search distance so very long shallow edges
// don't smear unboundedly. Tunable as constants below if a scene needs
// different defaults — at that point promote them to a small UBO.

layout(binding = 0) uniform sampler2D samplerColor;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

const float FXAA_SPAN_MAX           = 8.0;
const float FXAA_REDUCE_MUL         = 1.0 / 8.0;
const float FXAA_REDUCE_MIN         = 1.0 / 128.0;
const float FXAA_THRESHOLD_ABSOLUTE = 0.0312;
const float FXAA_THRESHOLD_RELATIVE = 0.125;

float luma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 rcpFrame = 1.0 / vec2(textureSize(samplerColor, 0));

    vec3 rgbC  = texture(samplerColor, inUV).rgb;
    vec3 rgbNW = textureOffset(samplerColor, inUV, ivec2(-1, -1)).rgb;
    vec3 rgbNE = textureOffset(samplerColor, inUV, ivec2( 1, -1)).rgb;
    vec3 rgbSW = textureOffset(samplerColor, inUV, ivec2(-1,  1)).rgb;
    vec3 rgbSE = textureOffset(samplerColor, inUV, ivec2( 1,  1)).rgb;

    float lC  = luma(rgbC);
    float lNW = luma(rgbNW);
    float lNE = luma(rgbNE);
    float lSW = luma(rgbSW);
    float lSE = luma(rgbSE);

    float lMin = min(lC, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lC, max(max(lNW, lNE), max(lSW, lSE)));
    float range = lMax - lMin;

    if (range < max(FXAA_THRESHOLD_ABSOLUTE, lMax * FXAA_THRESHOLD_RELATIVE))
    {
        outFragColor = vec4(rgbC, 1.0);
        return;
    }

    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * FXAA_REDUCE_MUL),
                          FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = clamp(dir * rcpDirMin,
                vec2(-FXAA_SPAN_MAX),
                vec2( FXAA_SPAN_MAX)) * rcpFrame;

    vec3 rgbA = 0.5 * (
        texture(samplerColor, inUV + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(samplerColor, inUV + dir * (2.0 / 3.0 - 0.5)).rgb);

    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(samplerColor, inUV + dir * -0.5).rgb +
        texture(samplerColor, inUV + dir *  0.5).rgb);

    float lB = luma(rgbB);
    if (lB < lMin || lB > lMax)
        outFragColor = vec4(rgbA, 1.0);
    else
        outFragColor = vec4(rgbB, 1.0);
}

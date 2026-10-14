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

// The taps are premultiplied by their alpha, so that a transparent background
// (the thumbnails') doesn't darken the silhouette, and the result is
// premultiplied: the alpha is 1 everywhere in the window, where this is the same.
// A neighbour, `offset` pixels away (textureOffset wants a constant).
vec4 fetch(vec2 uv, vec2 offset)
{
    vec4 c = texture(samplerColor, uv + offset / vec2(textureSize(samplerColor, 0)));
    return vec4(c.rgb * c.a, c.a);
}

vec4 fetch(vec2 uv)
{
    vec4 c = texture(samplerColor, uv);
    return vec4(c.rgb * c.a, c.a);
}

float luma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 rcpFrame = 1.0 / vec2(textureSize(samplerColor, 0));

    vec4 rgbC  = fetch(inUV);
    vec4 rgbNW = fetch(inUV, vec2(-1, -1));
    vec4 rgbNE = fetch(inUV, vec2( 1, -1));
    vec4 rgbSW = fetch(inUV, vec2(-1,  1));
    vec4 rgbSE = fetch(inUV, vec2( 1,  1));

    float lC  = luma(rgbC.rgb);
    float lNW = luma(rgbNW.rgb);
    float lNE = luma(rgbNE.rgb);
    float lSW = luma(rgbSW.rgb);
    float lSE = luma(rgbSE.rgb);

    float lMin = min(lC, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lC, max(max(lNW, lNE), max(lSW, lSE)));
    float range = lMax - lMin;

    if (range < max(FXAA_THRESHOLD_ABSOLUTE, lMax * FXAA_THRESHOLD_RELATIVE))
    {
        outFragColor = rgbC;
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

    vec4 rgbA = 0.5 * (
        fetch(inUV + dir * (1.0 / 3.0 - 0.5)) +
        fetch(inUV + dir * (2.0 / 3.0 - 0.5)));

    vec4 rgbB = rgbA * 0.5 + 0.25 * (
        fetch(inUV + dir * -0.5) +
        fetch(inUV + dir *  0.5));

    float lB = luma(rgbB.rgb);
    if (lB < lMin || lB > lMax)
        outFragColor = rgbA;
    else
        outFragColor = rgbB;
}

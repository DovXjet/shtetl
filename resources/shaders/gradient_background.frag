#version 450

// A vertical gradient behind the model: top at the top of the target. The
// colors are sRGB and blended as such (as in other viewers); the target is
// sRGB too, so the result is converted to linear first.

layout(binding = 0) uniform UBO
{
    vec4 top;
    vec4 bottom;
} ubo;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
    outFragColor = vec4(srgbToLinear(mix(ubo.top.rgb, ubo.bottom.rgb, clamp(inUV.y, 0.0, 1.0))), 1.0);
}

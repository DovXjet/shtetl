#version 450

// The composition of the ambient occlusion mode: the lit color of the G-buffer
// darkened by the occlusion, on a target that the FXAA pass then reads.

layout(binding = 0) uniform sampler2D samplerPosition;
layout(binding = 1) uniform sampler2D samplerColor;
layout(binding = 2) uniform sampler2D samplerAO;
layout(binding = 3) uniform UBO
{
    float strength; // 0: no darkening, 1: full
    float transparentBackground; // 1: no geometry is transparent black, not the background colors
    float pad1;
    float pad2;
    vec4 background;       // where there is no geometry: the top (sRGB)...
    vec4 backgroundBottom; // ...and the bottom of the gradient
} ubo;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
    if (texture(samplerPosition, inUV).a >= 0.0)
    {
        if (ubo.transparentBackground > 0.5)
        {
            outFragColor = vec4(0.0);
            return;
        }
        outFragColor = vec4(srgbToLinear(mix(ubo.background.rgb, ubo.backgroundBottom.rgb, clamp(inUV.y, 0.0, 1.0))), 1.0);
        return;
    }
    vec3 color = texture(samplerColor, inUV).rgb;
    float ao = texture(samplerAO, inUV).r;
    outFragColor = vec4(color * mix(1.0, ao, clamp(ubo.strength, 0.0, 1.0)), 1.0);
}

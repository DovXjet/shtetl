#version 450

// The last pass of the ambient occlusion mode: the lit color of the G-buffer
// darkened by the occlusion, on the swapchain. The background is left alone,
// as it is cleared there.

layout(binding = 0) uniform sampler2D samplerPosition;
layout(binding = 1) uniform sampler2D samplerColor;
layout(binding = 2) uniform sampler2D samplerAO;
layout(binding = 3) uniform UBO
{
    float strength; // 0: no darkening, 1: full
    float pad0;
    float pad1;
    float pad2;
} ubo;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

void main()
{
    if (texture(samplerPosition, inUV).a >= 0.0) discard;
    vec3 color = texture(samplerColor, inUV).rgb;
    float ao = texture(samplerAO, inUV).r;
    outFragColor = vec4(color * mix(1.0, ao, clamp(ubo.strength, 0.0, 1.0)), 1.0);
}

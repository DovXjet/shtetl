#version 450

// The 5x5 box blur that removes the noise of the SSAO pass.

layout(binding = 0) uniform sampler2D samplerSSAO;

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outFragColor;

void main()
{
    const int blurRange = 2;
    vec2 texelSize = 1.0 / vec2(textureSize(samplerSSAO, 0));
    float result = 0.0;
    for (int x = -blurRange; x <= blurRange; x++)
        for (int y = -blurRange; y <= blurRange; y++)
            result += texture(samplerSSAO, inUV + vec2(float(x), float(y)) * texelSize).r;
    outFragColor = result / float((2 * blurRange + 1) * (2 * blurRange + 1));
}

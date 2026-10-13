#version 450
#extension GL_ARB_separate_shader_objects : enable

// The lighting of XjetStudio's colored_mesh.frag (two lights and a specular
// term), with the faces lit from both sides.

layout(set = 1, binding = 10) uniform MaterialData
{
    vec4 ambientColor;
    vec4 diffuseColor;
    vec4 specularColor;
    vec4 emissiveColor;
    float shininess;
    float alphaMask;
    float alphaMaskCutoff;
} material;

layout(location = 0) in vec3 normal;
layout(location = 1) in vec3 pos;

layout(location = 0) out vec4 outColor;

const float lightStrength1 = 0.25;
const vec3 lightPos1 = vec3(-1e4, 0.5e4, 1e4);
const vec3 lightPos = vec3(0, 0, 5);

void main()
{
    vec3 N = normalize(normal);
    if (!gl_FrontFacing) N = -N;

    // First light
    vec3 L = normalize(lightPos - pos);
    vec3 R = reflect(-L, N);
    vec3 V = -normalize(pos);

    float Kd = max(dot(L, N), 0.0);
    float Ks = pow(max(dot(R, V), 0.0), material.shininess) * 0.6;

    // Second light
    L = normalize(lightPos1 - pos);
    R = reflect(-L, N);
    Kd += max(dot(L, N), 0.0) * lightStrength1;
    Ks += pow(max(dot(R, V), 0.0), material.shininess) * lightStrength1;

    outColor = vec4(material.ambientColor.xyz
                    + material.diffuseColor.xyz * Kd * 0.7
                    + material.specularColor.xyz * Ks * 0.02,
                    material.diffuseColor.w);
}

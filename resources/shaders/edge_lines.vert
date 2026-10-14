#version 450
#extension GL_ARB_separate_shader_objects : enable

// The edge lines over a compact mesh: the welded vertices carry the positions
// only, 24-bit fractions of the mesh's bounding box as in compact_mesh.vert
// (whose box transform is also applied to these).

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 modelView;
} pc;

layout(location = 0) in uvec3 vsg_PackedData;

layout(location = 0) out vec3 frag_vertex;

void main()
{
    vec3 objectPos = vec3(vsg_PackedData & 0x00FFFFFFu) / 16777215.0;
    vec4 viewPos = pc.modelView * vec4(objectPos, 1.0);
    gl_Position = pc.projection * viewPos;
    frag_vertex = viewPos.xyz;
}

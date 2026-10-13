#version 450
#extension GL_ARB_separate_shader_objects : enable

// Compact mesh vertex, ported from XjetStudio's colored_mesh_compact.vert:
// 12 bytes per vertex (see CompactMesh.cpp for the layout). The positions are
// 24-bit fractions of the mesh's bounding box, so the model matrix that
// carries this node's transform also has to undo that normalisation, and the
// normal is the flat face normal of the triangle, encoded octahedrally.

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 modelView;
} pc;

layout(location = 0) in uvec3 vsg_PackedData;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec3 frag_vertex;

vec3 decodeOctNormal(uint lowX, uint lowY, uint highX, uint highY)
{
    // Reassemble the 11-bit components (8 low + 3 high) and map them to [-1, 1].
    vec2 f = vec2(lowX | (highX << 8), lowY | (highY << 8)) / 2047.0;
    f = f * 2.0 - 1.0;

    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0)
    {
        vec2 sign_n = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * sign_n;
    }
    return normalize(n);
}

void main()
{
    // Word 0: [NormLowX (8) | PosX (24)]   Word 1: [NormLowY (8) | PosY (24)]
    // Word 2: [Bary (2) | NormHighY (3) | NormHighX (3) | PosZ (24)]
    vec3 objectPos = vec3(vsg_PackedData & 0x00FFFFFFu) / 16777215.0;

    uint zBits = vsg_PackedData.z >> 24;
    vec3 objectNormal = decodeOctNormal(vsg_PackedData.x >> 24, vsg_PackedData.y >> 24,
                                        zBits & 0x7u, (zBits >> 3) & 0x7u);

    vec4 viewPos = pc.modelView * vec4(objectPos, 1.0);
    gl_Position = pc.projection * viewPos;
    frag_vertex = viewPos.xyz;

    // The normal was encoded in the normalised box space, where it is
    // covariant: the inverse transpose of the model-view undoes the box's
    // (non-uniform) scale, which mat3(modelView) alone would not.
    frag_normal = normalize(transpose(inverse(mat3(pc.modelView))) * objectNormal);
}

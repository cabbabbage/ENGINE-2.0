cbuffer SpriteBatchUniform : register(b0, space1) {
    float4 u_vertex_uv[6];
    float4 u_modulate;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

VsOut main(uint vertex_id : SV_VertexID) {
    const float4 packed = u_vertex_uv[vertex_id];
    VsOut output_value;
    output_value.position = float4(packed.x, packed.y, 0.0, 1.0);
    output_value.uv = packed.zw;
    output_value.color = u_modulate;
    return output_value;
}

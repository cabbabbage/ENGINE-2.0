struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut main(uint vertex_id : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VsOut output_value;
    output_value.position = float4(positions[vertex_id], 0.0, 1.0);
    output_value.uv = uvs[vertex_id];
    return output_value;
}

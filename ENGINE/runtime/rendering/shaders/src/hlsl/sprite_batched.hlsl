float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return float4(uv.x, uv.y, 1.0 - uv.x, 1.0);
}

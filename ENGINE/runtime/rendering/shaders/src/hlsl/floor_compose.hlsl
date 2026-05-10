Texture2D u_floor : register(t0, space2);
SamplerState u_floor_sampler : register(s0, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 floor_color = u_floor.Sample(u_floor_sampler, uv);
    return float4(floor_color.rgb, 1.0);
}

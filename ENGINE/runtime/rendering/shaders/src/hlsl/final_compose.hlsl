Texture2D u_floor : register(t0, space2);
SamplerState u_floor_sampler : register(s0, space2);
Texture2D u_layers : register(t1, space2);
SamplerState u_layers_sampler : register(s1, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 floor_color = u_floor.Sample(u_floor_sampler, uv);
    float4 layer_color = u_layers.Sample(u_layers_sampler, uv);
    float layer_alpha = saturate(layer_color.a);
    float3 composed_rgb = layer_color.rgb + floor_color.rgb * (1.0 - layer_alpha);
    return float4(composed_rgb, 1.0);
}

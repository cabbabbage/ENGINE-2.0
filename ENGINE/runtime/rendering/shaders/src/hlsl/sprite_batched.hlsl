Texture2D u_sprite : register(t0, space2);
SamplerState u_sprite_sampler : register(s0, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float4 color : TEXCOORD1) : SV_Target0 {
    return u_sprite.Sample(u_sprite_sampler, uv) * color;
}

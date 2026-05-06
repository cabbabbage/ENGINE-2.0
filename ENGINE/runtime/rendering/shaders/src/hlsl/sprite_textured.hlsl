Texture2D u_scene : register(t0, space2);
SamplerState u_scene_sampler : register(s0, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return u_scene.Sample(u_scene_sampler, uv);
}

Texture2D u_scene : register(t0, space2);
SamplerState u_scene_sampler : register(s0, space2);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 sample_blur(float2 uv, float2 step_offset) {
    float4 result = u_scene.Sample(u_scene_sampler, uv) * 0.2270270270f;
    result += u_scene.Sample(u_scene_sampler, uv + step_offset * 1.3846153846f) * 0.3162162162f;
    result += u_scene.Sample(u_scene_sampler, uv - step_offset * 1.3846153846f) * 0.3162162162f;
    result += u_scene.Sample(u_scene_sampler, uv + step_offset * 3.2307692308f) * 0.0702702703f;
    result += u_scene.Sample(u_scene_sampler, uv - step_offset * 3.2307692308f) * 0.0702702703f;
    return result;
}

float4 main(VSOutput input) : SV_Target0 {
    float2 direction = input.color.xy;
    float blur_radius_px = max(input.color.z, 0.0f);
    if (blur_radius_px <= 0.0f || dot(direction, direction) <= 0.0f) {
        return u_scene.Sample(u_scene_sampler, input.uv);
    }

    uint width = 1;
    uint height = 1;
    u_scene.GetDimensions(width, height);
    float2 texel_size = 1.0f / float2(max(width, 1u), max(height, 1u));
    float2 normalized_direction = normalize(direction);
    float2 step_offset = normalized_direction * texel_size * blur_radius_px;
    return sample_blur(input.uv, step_offset);
}

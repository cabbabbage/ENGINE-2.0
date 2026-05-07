#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;

vec4 sample_blur(vec2 uv, vec2 step_offset) {
    vec4 result = texture(u_scene, uv) * 0.2270270270;
    result += texture(u_scene, uv + step_offset * 1.3846153846) * 0.3162162162;
    result += texture(u_scene, uv - step_offset * 1.3846153846) * 0.3162162162;
    result += texture(u_scene, uv + step_offset * 3.2307692308) * 0.0702702703;
    result += texture(u_scene, uv - step_offset * 3.2307692308) * 0.0702702703;
    return result;
}

void main() {
    vec2 direction = v_color.xy;
    float blur_radius_px = max(v_color.z, 0.0);
    if (blur_radius_px <= 0.0 || dot(direction, direction) <= 0.0) {
        out_color = texture(u_scene, v_uv);
        return;
    }

    vec2 texel_size = 1.0 / vec2(textureSize(u_scene, 0));
    vec2 normalized_direction = normalize(direction);
    vec2 step_offset = normalized_direction * texel_size * blur_radius_px;
    out_color = sample_blur(v_uv, step_offset);
}

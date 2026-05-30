#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_scene;
layout(set = 2, binding = 1) uniform sampler2D field_curvature_debug_or_blur_amount_texture;

layout(set = 1, binding = 0) uniform FinalLensUniforms {
    vec2 optical_center;
    vec2 texel_size;
    float barrel_distortion;
    float distortion_zoom_compensation;
    float vignette_strength;
    float vignette_radius;
    float vignette_softness;
    float vignette_depth_influence;
    float chromatic_aberration;
    float chromatic_edge_start;
    float chromatic_depth_influence;
    float edge_softness;
    float has_blur_amount_texture;
    float bloom_strength;
    float bloom_threshold;
    float bloom_radius;
    float halation_strength;
} u_final_lens;

float blur_amount_at(vec2 uv) {
    if (u_final_lens.has_blur_amount_texture <= 0.5) {
        return 0.0;
    }
    return clamp(texture(field_curvature_debug_or_blur_amount_texture, clamp(uv, vec2(0.0), vec2(1.0))).r, 0.0, 1.0);
}

vec2 safe_lens_uv(vec2 uv) {
    vec2 centered = uv - u_final_lens.optical_center;
    float r2 = dot(centered, centered);
    vec2 distorted = centered * (1.0 + u_final_lens.barrel_distortion * r2);
    vec2 compensated = u_final_lens.optical_center + distorted * u_final_lens.distortion_zoom_compensation;
    return clamp(compensated, vec2(0.001), vec2(0.999));
}

vec3 edge_softened_sample(vec2 uv, float radius, float blur_amount) {
    vec3 base = texture(u_scene, uv).rgb;
    float edge_gate = smoothstep(0.30, 0.95, radius);
    float softness = clamp(u_final_lens.edge_softness * edge_gate * (0.65 + 0.35 * blur_amount), 0.0, 1.0);
    if (softness <= 0.0001) {
        return base;
    }

    vec2 px = u_final_lens.texel_size * (1.0 + 2.0 * softness);
    vec3 blur = base * 0.40;
    blur += texture(u_scene, clamp(uv + vec2( px.x, 0.0), vec2(0.001), vec2(0.999))).rgb * 0.15;
    blur += texture(u_scene, clamp(uv + vec2(-px.x, 0.0), vec2(0.001), vec2(0.999))).rgb * 0.15;
    blur += texture(u_scene, clamp(uv + vec2(0.0,  px.y), vec2(0.001), vec2(0.999))).rgb * 0.15;
    blur += texture(u_scene, clamp(uv + vec2(0.0, -px.y), vec2(0.001), vec2(0.999))).rgb * 0.15;
    return mix(base, blur, softness);
}

void main() {
    vec2 centered = v_uv - u_final_lens.optical_center;
    float radius = length(centered);
    vec2 uv = safe_lens_uv(v_uv);
    float blur_amount = blur_amount_at(uv);

    float chroma_edge = smoothstep(u_final_lens.chromatic_edge_start, 1.0, radius);
    float chroma_depth = mix(1.0, blur_amount, clamp(u_final_lens.chromatic_depth_influence, 0.0, 1.0));
    vec2 chroma_dir = radius > 0.0001 ? centered / radius : vec2(0.0);
    vec2 chroma_offset = chroma_dir * u_final_lens.chromatic_aberration * chroma_edge * chroma_depth;

    vec3 color = edge_softened_sample(uv, radius, blur_amount);
    color.r = texture(u_scene, clamp(uv + chroma_offset, vec2(0.001), vec2(0.999))).r;
    color.b = texture(u_scene, clamp(uv - chroma_offset, vec2(0.001), vec2(0.999))).b;

    float vignette_depth = mix(1.0, 1.0 + blur_amount, clamp(u_final_lens.vignette_depth_influence, 0.0, 1.0));
    float vignette = smoothstep(
        u_final_lens.vignette_radius,
        u_final_lens.vignette_radius + max(u_final_lens.vignette_softness, 0.001),
        radius * vignette_depth);
    float falloff = 1.0 - u_final_lens.vignette_strength * vignette * (0.85 + 0.15 * radius);
    color *= clamp(falloff, 0.0, 1.0);

    out_color = vec4(color, 1.0);
}

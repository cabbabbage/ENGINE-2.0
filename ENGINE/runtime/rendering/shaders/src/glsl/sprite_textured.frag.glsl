#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 albedo = vec3(v_uv.x, v_uv.y, 1.0 - v_uv.x);
    vec3 flatNormal = vec3(0.0, 0.0, 1.0);
    vec3 pseudoNormal = normalize(vec3(v_uv * 2.0 - 1.0, 1.0));
    bool hasNormal = false;
    vec3 N = hasNormal ? pseudoNormal : flatNormal;
    float roughness = 1.0; // diffuse-only fallback when channel is missing
    float heightBias = 0.0;

    vec3 L = normalize(vec3(0.4, 0.5, 1.0 + heightBias));
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(L + V);

    float lambert = max(dot(N, L), 0.0);
    float specPower = mix(8.0, 64.0, 1.0 - roughness);
    float spec = pow(max(dot(N, H), 0.0), specPower) * (1.0 - roughness);

    vec3 lit = albedo * (0.15 + lambert) + vec3(spec);
    out_color = vec4(lit, 1.0);
}

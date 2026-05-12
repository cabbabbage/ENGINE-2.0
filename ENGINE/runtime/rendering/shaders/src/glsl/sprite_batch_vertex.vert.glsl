#version 450

layout(set = 1, binding = 0) uniform SpriteBatchUniform {
    vec4 vertex_uv[6];
    vec4 modulate;
} ubo;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    vec4 packed = ubo.vertex_uv[gl_VertexIndex];
    gl_Position = vec4(packed.xy, 0.0, 1.0);
    v_uv = packed.zw;
    v_color = ubo.modulate;
}

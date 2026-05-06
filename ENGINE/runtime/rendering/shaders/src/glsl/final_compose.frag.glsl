#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_floor;
layout(set = 2, binding = 1) uniform sampler2D u_layers;

void main() {
    vec4 floor_color = texture(u_floor, v_uv);
    vec4 layer_color = texture(u_layers, v_uv);
    out_color = mix(floor_color, layer_color, layer_color.a);
}

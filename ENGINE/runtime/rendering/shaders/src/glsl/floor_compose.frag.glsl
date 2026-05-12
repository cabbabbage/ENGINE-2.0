#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_floor;

void main() {
    vec4 floor_color = texture(u_floor, v_uv);
    out_color = vec4(floor_color.rgb, 1.0);
}

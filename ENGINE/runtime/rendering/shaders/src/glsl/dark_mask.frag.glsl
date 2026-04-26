#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_uv.x, v_uv.y, 1.0 - v_uv.x, 1.0);
}

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) buffer LightBins {
    uint data[];
} g_light_bins;

void main() {
    uint linear_index = gl_GlobalInvocationID.y * 1024u + gl_GlobalInvocationID.x;
    g_light_bins.data[linear_index] = linear_index;
}

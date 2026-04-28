#pragma once

#include <SDL3/SDL.h>

#include <array>

namespace render_sink {

constexpr int kMaxClippedVertices = 6;
constexpr int kMaxClippedIndices = 12;

struct ClipResult {
    std::array<SDL_Vertex, kMaxClippedVertices> vertices{};
    std::array<int, kMaxClippedIndices> indices{};
    int vertex_count = 0;
    int index_count = 0;
    bool valid = false;
    bool clipped = false;
    bool fully_clipped = false;
};

ClipResult clip_quad_against_horizontal_sink_line(const SDL_Vertex (&quad)[4],
                                                  float sink_line_y,
                                                  float epsilon = 1.0e-4f);

} // namespace render_sink

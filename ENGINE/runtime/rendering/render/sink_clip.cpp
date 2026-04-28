#include "rendering/render/sink_clip.hpp"

#include <algorithm>
#include <cmath>

namespace render_sink {
namespace {

inline bool is_inside_sink_line(const SDL_Vertex& v, float sink_line_y, float epsilon) {
    return v.position.y <= (sink_line_y + epsilon);
}

inline float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

SDL_Vertex interpolate_on_sink_line(const SDL_Vertex& a,
                                    const SDL_Vertex& b,
                                    float sink_line_y,
                                    float epsilon) {
    SDL_Vertex out = a;
    const float dy = b.position.y - a.position.y;
    float t = 0.0f;
    if (std::fabs(dy) > epsilon) {
        t = clamp01((sink_line_y - a.position.y) / dy);
    }

    out.position.x = lerp(a.position.x, b.position.x, t);
    out.position.y = sink_line_y;
    out.color.r = lerp(a.color.r, b.color.r, t);
    out.color.g = lerp(a.color.g, b.color.g, t);
    out.color.b = lerp(a.color.b, b.color.b, t);
    out.color.a = lerp(a.color.a, b.color.a, t);
    out.tex_coord.x = lerp(a.tex_coord.x, b.tex_coord.x, t);
    out.tex_coord.y = lerp(a.tex_coord.y, b.tex_coord.y, t);
    return out;
}

bool nearly_equal_vertex(const SDL_Vertex& a, const SDL_Vertex& b, float epsilon) {
    return std::fabs(a.position.x - b.position.x) <= epsilon &&
           std::fabs(a.position.y - b.position.y) <= epsilon &&
           std::fabs(a.tex_coord.x - b.tex_coord.x) <= epsilon &&
           std::fabs(a.tex_coord.y - b.tex_coord.y) <= epsilon;
}

void push_unique_vertex(std::array<SDL_Vertex, 8>& vertices,
                        int& vertex_count,
                        const SDL_Vertex& vertex,
                        float epsilon) {
    if (vertex_count > 0 && nearly_equal_vertex(vertices[vertex_count - 1], vertex, epsilon)) {
        return;
    }
    if (vertex_count < static_cast<int>(vertices.size())) {
        vertices[vertex_count++] = vertex;
    }
}

} // namespace

ClipResult clip_quad_against_horizontal_sink_line(const SDL_Vertex (&quad)[4],
                                                  float sink_line_y,
                                                  float epsilon) {
    ClipResult out{};
    if (!std::isfinite(sink_line_y)) {
        return out;
    }
    const float safe_epsilon = (std::isfinite(epsilon) && epsilon > 0.0f) ? epsilon : 1.0e-4f;

    bool all_inside = true;
    bool all_outside = true;
    for (const SDL_Vertex& v : quad) {
        const bool inside = is_inside_sink_line(v, sink_line_y, safe_epsilon);
        all_inside = all_inside && inside;
        all_outside = all_outside && !inside;
    }

    if (all_outside) {
        out.fully_clipped = true;
        return out;
    }

    if (all_inside) {
        out.vertex_count = 4;
        out.index_count = 6;
        out.vertices[0] = quad[0];
        out.vertices[1] = quad[1];
        out.vertices[2] = quad[2];
        out.vertices[3] = quad[3];
        out.indices[0] = 0;
        out.indices[1] = 1;
        out.indices[2] = 2;
        out.indices[3] = 0;
        out.indices[4] = 2;
        out.indices[5] = 3;
        out.valid = true;
        out.clipped = false;
        return out;
    }

    std::array<SDL_Vertex, 8> clipped_vertices{};
    int clipped_count = 0;

    SDL_Vertex prev = quad[3];
    bool prev_inside = is_inside_sink_line(prev, sink_line_y, safe_epsilon);
    for (int i = 0; i < 4; ++i) {
        const SDL_Vertex curr = quad[i];
        const bool curr_inside = is_inside_sink_line(curr, sink_line_y, safe_epsilon);
        if (prev_inside && curr_inside) {
            push_unique_vertex(clipped_vertices, clipped_count, curr, safe_epsilon);
        } else if (prev_inside && !curr_inside) {
            push_unique_vertex(clipped_vertices,
                               clipped_count,
                               interpolate_on_sink_line(prev, curr, sink_line_y, safe_epsilon),
                               safe_epsilon);
        } else if (!prev_inside && curr_inside) {
            push_unique_vertex(clipped_vertices,
                               clipped_count,
                               interpolate_on_sink_line(prev, curr, sink_line_y, safe_epsilon),
                               safe_epsilon);
            push_unique_vertex(clipped_vertices, clipped_count, curr, safe_epsilon);
        }
        prev = curr;
        prev_inside = curr_inside;
    }

    if (clipped_count > 1 && nearly_equal_vertex(clipped_vertices[0], clipped_vertices[clipped_count - 1], safe_epsilon)) {
        --clipped_count;
    }

    if (clipped_count < 3 || clipped_count > kMaxClippedVertices) {
        out.fully_clipped = true;
        return out;
    }

    out.vertex_count = clipped_count;
    for (int i = 0; i < clipped_count; ++i) {
        out.vertices[i] = clipped_vertices[i];
    }

    int index_count = 0;
    for (int i = 1; i < clipped_count - 1; ++i) {
        if (index_count + 3 > kMaxClippedIndices) {
            break;
        }
        out.indices[index_count++] = 0;
        out.indices[index_count++] = i;
        out.indices[index_count++] = i + 1;
    }

    if (index_count < 3) {
        out.fully_clipped = true;
        out.vertex_count = 0;
        return out;
    }

    out.index_count = index_count;
    out.valid = true;
    out.clipped = true;
    return out;
}

} // namespace render_sink

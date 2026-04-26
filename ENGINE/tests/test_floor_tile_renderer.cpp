#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/floor_composer.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class ScopedRenderer {
public:
    ScopedRenderer() {
        if (!video_.initialized()) {
            return;
        }
        window_ = SDL_CreateWindow("floor_tile_renderer_tests", 256, 256, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ != nullptr; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

SDL_Texture* create_target_texture(SDL_Renderer* renderer, int width, int height) {
    if (!renderer || width <= 0 || height <= 0) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width,
                                             height);
    if (!texture) {
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

bool clear_texture(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Color color) {
    if (!renderer || !texture) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
    return true;
}

bool fill_texture_rect(SDL_Renderer* renderer,
                       SDL_Texture* texture,
                       int x,
                       int y,
                       int w,
                       int h,
                       SDL_Color color) {
    if (!renderer || !texture || w <= 0 || h <= 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(w),
        static_cast<float>(h)
    };
    const bool ok = SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderTarget(renderer, previous_target);
    return ok;
}

bool read_pixel(SDL_Renderer* renderer, SDL_Texture* texture, int x, int y, SDL_Color& out_color) {
    out_color = SDL_Color{0, 0, 0, 0};
    if (!renderer || !texture || x < 0 || y < 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    const SDL_Rect pixel_rect{x, y, 1, 1};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &pixel_rect);
    SDL_SetRenderTarget(renderer, previous_target);
    if (!captured || !captured->pixels) {
        if (captured) {
            SDL_DestroySurface(captured);
        }
        return false;
    }

    const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(captured->format);
    if (!format) {
        SDL_DestroySurface(captured);
        return false;
    }

    const Uint32 pixel = *static_cast<const Uint32*>(captured->pixels);
    SDL_GetRGBA(pixel,
                format,
                SDL_GetSurfacePalette(captured),
                &out_color.r,
                &out_color.g,
                &out_color.b,
                &out_color.a);
    SDL_DestroySurface(captured);
    return true;
}

Area make_starting_area() {
    std::vector<SDL_Point> corners{
        SDL_Point{-4096, -4096},
        SDL_Point{4096, -4096},
        SDL_Point{4096, 4096},
        SDL_Point{-4096, 4096}
    };
    return Area("floor_tile_renderer_test_start", corners, 0);
}

bool project_floor_grid_point_to_screen(const WarpedScreenGrid& cam,
                                        SDL_Point world_pos,
                                        SDL_FPoint& out_screen) {
    SDL_FPoint linear_screen{};
    if (!cam.project_world_point(SDL_FPoint{static_cast<float>(world_pos.x), 0.0f},
                                 static_cast<float>(world_pos.y),
                                 linear_screen) ||
        !std::isfinite(linear_screen.x) ||
        !std::isfinite(linear_screen.y)) {
        return false;
    }
    linear_screen.y = cam.warp_floor_screen_y(0.0f, linear_screen.y);
    if (!std::isfinite(linear_screen.y)) {
        return false;
    }
    out_screen = linear_screen;
    return true;
}

constexpr float kQuadEpsilon = 1.0e-5f;

float cross(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool on_segment(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& p) {
    return p.x >= std::min(a.x, b.x) - kQuadEpsilon &&
           p.x <= std::max(a.x, b.x) + kQuadEpsilon &&
           p.y >= std::min(a.y, b.y) - kQuadEpsilon &&
           p.y <= std::max(a.y, b.y) + kQuadEpsilon;
}

bool segments_intersect(const SDL_FPoint& a,
                        const SDL_FPoint& b,
                        const SDL_FPoint& c,
                        const SDL_FPoint& d) {
    const float d1 = cross(a, b, c);
    const float d2 = cross(a, b, d);
    const float d3 = cross(c, d, a);
    const float d4 = cross(c, d, b);

    const bool d1_zero = std::abs(d1) <= kQuadEpsilon;
    const bool d2_zero = std::abs(d2) <= kQuadEpsilon;
    const bool d3_zero = std::abs(d3) <= kQuadEpsilon;
    const bool d4_zero = std::abs(d4) <= kQuadEpsilon;

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
        ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }

    return (d1_zero && on_segment(a, b, c)) ||
           (d2_zero && on_segment(a, b, d)) ||
           (d3_zero && on_segment(c, d, a)) ||
           (d4_zero && on_segment(c, d, b));
}

bool is_convex_quad(const std::array<SDL_FPoint, 4>& points) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float area = cross(points[i], points[(i + 1) % 4], points[(i + 2) % 4]);
        if (std::abs(area) <= kQuadEpsilon) {
            continue;
        }
        if (sign == 0.0f) {
            sign = area;
        } else if ((sign > 0.0f) != (area > 0.0f)) {
            return false;
        }
    }
    return true;
}

void enforce_trapezoid(std::array<SDL_FPoint, 4>& points) {
    const bool intersects = segments_intersect(points[0], points[1], points[2], points[3]) ||
                            segments_intersect(points[1], points[2], points[3], points[0]);
    if (!intersects && is_convex_quad(points)) {
        return;
    }

    std::array<int, 4> idx{0, 1, 2, 3};
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (points[a].y != points[b].y) {
            return points[a].y < points[b].y;
        }
        return points[a].x < points[b].x;
    });

    int tl = idx[0];
    int tr = idx[1];
    int bl = idx[2];
    int br = idx[3];
    if (points[tl].x > points[tr].x) std::swap(tl, tr);
    if (points[bl].x > points[br].x) std::swap(bl, br);
    points = {points[tl], points[tr], points[br], points[bl]};
}

std::array<SDL_FPoint, 4> project_tile_quad(const WarpedScreenGrid& cam, const SDL_Rect& world_rect) {
    SDL_FPoint stl{};
    SDL_FPoint str{};
    SDL_FPoint sbr{};
    SDL_FPoint sbl{};
    project_floor_grid_point_to_screen(cam, SDL_Point{world_rect.x, world_rect.y}, stl);
    project_floor_grid_point_to_screen(cam, SDL_Point{world_rect.x + world_rect.w, world_rect.y}, str);
    project_floor_grid_point_to_screen(cam, SDL_Point{world_rect.x + world_rect.w, world_rect.y + world_rect.h}, sbr);
    project_floor_grid_point_to_screen(cam, SDL_Point{world_rect.x, world_rect.y + world_rect.h}, sbl);
    std::array<SDL_FPoint, 4> points{stl, str, sbr, sbl};
    enforce_trapezoid(points);
    return points;
}

bool point_in_triangle(SDL_FPoint p, SDL_FPoint a, SDL_FPoint b, SDL_FPoint c) {
    const float s1 = cross(a, b, p);
    const float s2 = cross(b, c, p);
    const float s3 = cross(c, a, p);
    const bool has_neg = (s1 < -kQuadEpsilon) || (s2 < -kQuadEpsilon) || (s3 < -kQuadEpsilon);
    const bool has_pos = (s1 > kQuadEpsilon) || (s2 > kQuadEpsilon) || (s3 > kQuadEpsilon);
    return !(has_neg && has_pos);
}

bool point_in_quad(SDL_FPoint p, const std::array<SDL_FPoint, 4>& quad) {
    return point_in_triangle(p, quad[0], quad[1], quad[2]) ||
           point_in_triangle(p, quad[0], quad[2], quad[3]);
}

bool render_tiles_to_target(SDL_Renderer* renderer,
                            const WarpedScreenGrid& cam,
                            world::WorldGrid& grid,
                            SDL_Texture* target) {
    if (!renderer || !target) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, target)) {
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    GridTileRenderer renderer_tiles(nullptr);
    renderer_tiles.render(renderer, cam, grid, nullptr);
    SDL_SetRenderTarget(renderer, previous_target);
    return true;
}

int luminance_u8(const SDL_Color& color) {
    return static_cast<int>(std::lround(0.2126 * static_cast<double>(color.r) +
                                        0.7152 * static_cast<double>(color.g) +
                                        0.0722 * static_cast<double>(color.b)));
}

} // namespace

TEST_CASE("GridTileRenderer avoids high-frequency scanline striping on simple half-split textures") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kOutputW = 256;
    constexpr int kOutputH = 256;
    SDL_Texture* output = create_target_texture(renderer, kOutputW, kOutputH);
    REQUIRE(output != nullptr);

    SDL_Texture* tile_texture = create_target_texture(renderer, 8, 8);
    REQUIRE(tile_texture != nullptr);
    REQUIRE(clear_texture(renderer, tile_texture, SDL_Color{0, 0, 255, 255}));
    REQUIRE(fill_texture_rect(renderer, tile_texture, 0, 0, 8, 4, SDL_Color{255, 0, 0, 255}));
    SDL_SetTextureScaleMode(tile_texture, SDL_SCALEMODE_LINEAR);

    world::WorldGrid grid;
    grid.set_grid_resolution(6);
    world::Chunk* chunk = grid.get_or_create_chunk_ij(0, 0);
    REQUIRE(chunk != nullptr);

    const SDL_Rect world_rect{-64, -48, 128, 96};
    chunk->tiles.push_back(GridTile{world_rect, tile_texture});
    grid.update_active_chunks(world::GridBounds::from_xywh(-256, -256, 512, 512), 0);

    WarpedScreenGrid cam(kOutputW, kOutputH, make_starting_area());
    REQUIRE(render_tiles_to_target(renderer, cam, grid, output));

    const std::array<SDL_FPoint, 4> quad = project_tile_quad(cam, world_rect);
    const SDL_FPoint center{
        (quad[0].x + quad[1].x + quad[2].x + quad[3].x) * 0.25f,
        (quad[0].y + quad[1].y + quad[2].y + quad[3].y) * 0.25f
    };

    float min_y = quad[0].y;
    float max_y = quad[0].y;
    for (const SDL_FPoint& p : quad) {
        min_y = std::min(min_y, p.y);
        max_y = std::max(max_y, p.y);
    }

    std::vector<int> states;
    bool saw_red = false;
    bool saw_blue = false;
    for (int i = 0; i < 64; ++i) {
        const float t = static_cast<float>(i) / 63.0f;
        SDL_FPoint sample{
            center.x,
            min_y + (max_y - min_y) * t
        };
        if (!point_in_quad(sample, quad)) {
            continue;
        }
        const int sx = static_cast<int>(std::lround(sample.x));
        const int sy = static_cast<int>(std::lround(sample.y));
        if (sx < 0 || sx >= kOutputW || sy < 0 || sy >= kOutputH) {
            continue;
        }

        SDL_Color color{};
        REQUIRE(read_pixel(renderer, output, sx, sy, color));
        if (color.r > color.b + 16) {
            states.push_back(1);
            saw_red = true;
        } else if (color.b > color.r + 16) {
            states.push_back(-1);
            saw_blue = true;
        }
    }

    REQUIRE(saw_red);
    REQUIRE(saw_blue);
    REQUIRE(states.size() >= 8);

    int transitions = 0;
    for (std::size_t i = 1; i < states.size(); ++i) {
        if (states[i] != states[i - 1]) {
            ++transitions;
        }
    }
    CHECK(transitions <= 4);

    SDL_DestroyTexture(output);
}

TEST_CASE("GridTileRenderer uses normalized UVs with correct quadrant mapping") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kOutputW = 256;
    constexpr int kOutputH = 256;
    SDL_Texture* output = create_target_texture(renderer, kOutputW, kOutputH);
    REQUIRE(output != nullptr);

    SDL_Texture* tile_texture = create_target_texture(renderer, 4, 4);
    REQUIRE(tile_texture != nullptr);
    REQUIRE(clear_texture(renderer, tile_texture, SDL_Color{0, 0, 0, 255}));
    REQUIRE(fill_texture_rect(renderer, tile_texture, 0, 0, 2, 2, SDL_Color{255, 0, 0, 255}));
    REQUIRE(fill_texture_rect(renderer, tile_texture, 2, 0, 2, 2, SDL_Color{0, 255, 0, 255}));
    REQUIRE(fill_texture_rect(renderer, tile_texture, 2, 2, 2, 2, SDL_Color{0, 0, 255, 255}));
    REQUIRE(fill_texture_rect(renderer, tile_texture, 0, 2, 2, 2, SDL_Color{255, 255, 0, 255}));
    SDL_SetTextureScaleMode(tile_texture, SDL_SCALEMODE_NEAREST);

    world::WorldGrid grid;
    grid.set_grid_resolution(6);
    world::Chunk* chunk = grid.get_or_create_chunk_ij(0, 0);
    REQUIRE(chunk != nullptr);

    const SDL_Rect world_rect{-64, -48, 128, 96};
    chunk->tiles.push_back(GridTile{world_rect, tile_texture});
    grid.update_active_chunks(world::GridBounds::from_xywh(-256, -256, 512, 512), 0);

    WarpedScreenGrid cam(kOutputW, kOutputH, make_starting_area());
    REQUIRE(render_tiles_to_target(renderer, cam, grid, output));

    const std::array<SDL_FPoint, 4> quad = project_tile_quad(cam, world_rect);
    const SDL_FPoint center{
        (quad[0].x + quad[1].x + quad[2].x + quad[3].x) * 0.25f,
        (quad[0].y + quad[1].y + quad[2].y + quad[3].y) * 0.25f
    };

    auto sample_toward_center = [&](const SDL_FPoint& corner) {
        SDL_FPoint p{
            corner.x + (center.x - corner.x) * 0.20f,
            corner.y + (center.y - corner.y) * 0.20f
        };
        return SDL_Point{
            static_cast<int>(std::lround(p.x)),
            static_cast<int>(std::lround(p.y))
        };
    };

    const SDL_Point tl = sample_toward_center(quad[0]);
    const SDL_Point tr = sample_toward_center(quad[1]);
    const SDL_Point br = sample_toward_center(quad[2]);
    const SDL_Point bl = sample_toward_center(quad[3]);
    const SDL_Point c{
        static_cast<int>(std::lround(center.x)),
        static_cast<int>(std::lround(center.y))
    };

    SDL_Color c_tl{};
    SDL_Color c_tr{};
    SDL_Color c_br{};
    SDL_Color c_bl{};
    SDL_Color c_center{};
    REQUIRE(read_pixel(renderer, output, tl.x, tl.y, c_tl));
    REQUIRE(read_pixel(renderer, output, tr.x, tr.y, c_tr));
    REQUIRE(read_pixel(renderer, output, br.x, br.y, c_br));
    REQUIRE(read_pixel(renderer, output, bl.x, bl.y, c_bl));
    REQUIRE(read_pixel(renderer, output, c.x, c.y, c_center));

    CHECK(c_tl.r > c_tl.g);
    CHECK(c_tl.r > c_tl.b);
    CHECK(c_tr.g > c_tr.r);
    CHECK(c_tr.g > c_tr.b);
    CHECK(c_br.b > c_br.r);
    CHECK(c_br.b > c_br.g);
    CHECK(c_bl.r > c_bl.b);
    CHECK(c_bl.g > c_bl.b);
    CHECK(c_center.r > 16);
    CHECK(c_center.g > 16);
    CHECK(c_center.b > 16);

    SDL_DestroyTexture(output);
}

TEST_CASE("GridTileRenderer keeps seam continuity for adjacent tiles before and after small camera pan") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kOutputW = 256;
    constexpr int kOutputH = 256;
    SDL_Texture* output = create_target_texture(renderer, kOutputW, kOutputH);
    REQUIRE(output != nullptr);

    world::WorldGrid grid;
    grid.set_grid_resolution(6);
    world::Chunk* chunk = grid.get_or_create_chunk_ij(0, 0);
    REQUIRE(chunk != nullptr);

    SDL_Texture* tile_left = create_target_texture(renderer, 8, 8);
    SDL_Texture* tile_right = create_target_texture(renderer, 8, 8);
    REQUIRE(tile_left != nullptr);
    REQUIRE(tile_right != nullptr);
    REQUIRE(clear_texture(renderer, tile_left, SDL_Color{90, 120, 160, 255}));
    REQUIRE(clear_texture(renderer, tile_right, SDL_Color{90, 120, 160, 255}));
    SDL_SetTextureScaleMode(tile_left, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(tile_right, SDL_SCALEMODE_LINEAR);

    const SDL_Rect left_rect{-128, -48, 128, 96};
    const SDL_Rect right_rect{0, -48, 128, 96};
    chunk->tiles.push_back(GridTile{left_rect, tile_left});
    chunk->tiles.push_back(GridTile{right_rect, tile_right});
    grid.update_active_chunks(world::GridBounds::from_xywh(-320, -256, 640, 512), 0);

    auto seam_delta = [&](WarpedScreenGrid& cam) {
        REQUIRE(render_tiles_to_target(renderer, cam, grid, output));

        SDL_FPoint seam_top{};
        SDL_FPoint seam_bottom{};
        REQUIRE(project_floor_grid_point_to_screen(cam, SDL_Point{0, left_rect.y}, seam_top));
        REQUIRE(project_floor_grid_point_to_screen(cam, SDL_Point{0, left_rect.y + left_rect.h}, seam_bottom));

        SDL_FPoint dir{
            seam_bottom.x - seam_top.x,
            seam_bottom.y - seam_top.y
        };
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        REQUIRE(len > 1.0f);
        dir.x /= len;
        dir.y /= len;
        SDL_FPoint normal{-dir.y, dir.x};

        int max_delta = 0;
        int sample_count = 0;
        for (int i = 0; i < 24; ++i) {
            const float t = (static_cast<float>(i) + 0.5f) / 24.0f;
            SDL_FPoint base{
                seam_top.x + (seam_bottom.x - seam_top.x) * t,
                seam_top.y + (seam_bottom.y - seam_top.y) * t
            };
            SDL_FPoint p_left{
                base.x - normal.x * 2.0f,
                base.y - normal.y * 2.0f
            };
            SDL_FPoint p_right{
                base.x + normal.x * 2.0f,
                base.y + normal.y * 2.0f
            };

            const int lx = static_cast<int>(std::lround(p_left.x));
            const int ly = static_cast<int>(std::lround(p_left.y));
            const int rx = static_cast<int>(std::lround(p_right.x));
            const int ry = static_cast<int>(std::lround(p_right.y));
            if (lx < 0 || lx >= kOutputW || ly < 0 || ly >= kOutputH ||
                rx < 0 || rx >= kOutputW || ry < 0 || ry >= kOutputH) {
                continue;
            }

            SDL_Color c_left{};
            SDL_Color c_right{};
            REQUIRE(read_pixel(renderer, output, lx, ly, c_left));
            REQUIRE(read_pixel(renderer, output, rx, ry, c_right));
            const int delta = std::abs(luminance_u8(c_left) - luminance_u8(c_right));
            max_delta = std::max(max_delta, delta);
            ++sample_count;
        }

        REQUIRE(sample_count >= 8);
        return max_delta;
    };

    WarpedScreenGrid cam(kOutputW, kOutputH, make_starting_area());
    const int delta_before = seam_delta(cam);

    const SDL_Point center = cam.get_screen_center();
    cam.set_screen_center(SDL_Point{center.x + 1, center.y}, true);
    const int delta_after = seam_delta(cam);

    CHECK(delta_before <= 48);
    CHECK(delta_after <= 48);
    CHECK(std::abs(delta_after - delta_before) <= 20);

    SDL_DestroyTexture(output);
}

#include "rendering/render/sprite_floor_clip.hpp"

#include <cassert>
#include <cmath>

namespace {

bool near(float a, float b, float epsilon = 1.0e-5f) {
    return std::fabs(a - b) <= epsilon;
}

void test_clip_math() {
    {
        const auto clip = render_floor_clip::compute_floor_clip({25.0f, 1.0f, 100.0f});
        assert(clip.valid);
        assert(clip.visibility == render_floor_clip::Visibility::FullyVisible);
        assert(near(clip.visible_v, 1.0f));
    }
    {
        const auto clip = render_floor_clip::compute_floor_clip({-20.0f, 1.0f, 100.0f});
        assert(clip.valid);
        assert(clip.visibility == render_floor_clip::Visibility::PartiallyVisible);
        assert(near(clip.visible_v, 0.8f));
    }
    {
        const auto clip = render_floor_clip::compute_floor_clip({-120.0f, 1.0f, 100.0f});
        assert(clip.valid);
        assert(clip.visibility == render_floor_clip::Visibility::FullyBuried);
        assert(near(clip.visible_v, 0.0f));
    }
    {
        const auto clip = render_floor_clip::compute_floor_clip({-20.0f, 0.5f, 100.0f});
        assert(clip.valid);
        assert(clip.visibility == render_floor_clip::Visibility::PartiallyVisible);
        assert(near(clip.visible_v, 0.3f));
    }
}

void test_atlas_and_flip_mapping() {
    const render_floor_clip::AtlasUvRect atlas{0.25f, 0.5f, 0.75f, 1.0f};
    {
        const SDL_FPoint uv = render_floor_clip::map_local_uv_to_atlas({0.25f, 0.2f}, atlas, false, false);
        assert(near(uv.x, 0.375f));
        assert(near(uv.y, 0.6f));
    }
    {
        const SDL_FPoint uv = render_floor_clip::map_local_uv_to_atlas({0.25f, 0.2f}, atlas, true, false);
        assert(near(uv.x, 0.625f));
        assert(near(uv.y, 0.6f));
    }
    {
        const SDL_FPoint uv = render_floor_clip::map_local_uv_to_atlas({0.25f, 0.2f}, atlas, false, true);
        assert(near(uv.x, 0.375f));
        assert(near(uv.y, 0.9f));
    }
}

} // namespace

int main() {
    test_clip_math();
    test_atlas_and_flip_mapping();
    return 0;
}

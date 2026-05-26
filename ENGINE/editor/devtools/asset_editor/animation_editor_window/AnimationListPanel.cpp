#include "AnimationListPanel.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "AnimationDocument.hpp"
#include "EditorUIPrimitives.hpp"
#include "PreviewProvider.hpp"
#include "string_utils.hpp"
#include "dm_icons.hpp"
#include "dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include <nlohmann/json.hpp>

namespace {
#ifndef ANIMATION_LIST_PANEL_ROW_UPDATE_DEBUG
#define ANIMATION_LIST_PANEL_ROW_UPDATE_DEBUG 0
#endif

constexpr int kRowHeight = 84;
constexpr int kStickyHeaderHeight = 56;

constexpr int kIndentPerLevel = 16;

constexpr float kMinSizeFactor = 0.60f;

float size_factor_for_level(int level) {
    if (level <= 0) {
        return 1.0f;
    }
    switch (level) {
        case 1:

            return 0.85f;
        case 2:
            return 0.75f;
        case 3:
            return 0.65f;
        default:
            return kMinSizeFactor;
    }
}

int row_height_for_level(int level) {
    float factor = size_factor_for_level(level);
    int height = static_cast<int>(std::round(kRowHeight * factor));
    return std::max(1, height);
}

int indent_for_level(int level) {
    if (level <= 0) {
        return 0;
    }
    return level * kIndentPerLevel;
}

SDL_Point event_point(const SDL_Event& e) {
    SDL_Point p{0, 0};
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        p.x = e.motion.x;
        p.y = e.motion.y;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        p.x = e.button.x;
        p.y = e.button.y;
    }
    return p;
}

bool rects_intersect(const SDL_Rect& a, const SDL_Rect& b) {
    SDL_Rect result{};
    return SDL_GetRectIntersection(&a, &b, &result);
}

class ClipRectGuard {
  public:
    explicit ClipRectGuard(SDL_Renderer* renderer) : renderer_(renderer) {
        if (renderer_) {
            had_clip_ = SDL_RenderClipEnabled(renderer_);
            if (had_clip_) {
                SDL_GetRenderClipRect(renderer_, &previous_clip_);
            }
        }
    }

    ~ClipRectGuard() {
        if (!renderer_) {
            return;
        }
        if (had_clip_) {
            SDL_SetRenderClipRect(renderer_, &previous_clip_);
        } else {
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
    }

  private:
    SDL_Renderer* renderer_ = nullptr;
    SDL_Rect previous_clip_{0, 0, 0, 0};
    bool had_clip_ = false;
};

int resolve_wheel_delta(const SDL_MouseWheelEvent& wheel) {
    int delta = wheel.integer_y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
    }
    if (delta == 0) {
        float precise = wheel.y;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            precise = -precise;
        }
        delta = static_cast<int>(std::round(precise));
        if (delta == 0 && precise != 0.0f) {
            delta = precise > 0.0f ? 1 : -1;
        }
    }
    return delta;
}

SDL_Color hsv_to_rgb(float hue, float saturation, float value) {
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    saturation = std::clamp(saturation, 0.0f, 1.0f);
    value = std::clamp(value, 0.0f, 1.0f);

    const float chroma = value * saturation;
    const float h_prime = hue / 60.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(h_prime, 2.0f) - 1.0f));

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (0.0f <= h_prime && h_prime < 1.0f) { r = chroma; g = x; }
    else if (1.0f <= h_prime && h_prime < 2.0f) { r = x; g = chroma; }
    else if (2.0f <= h_prime && h_prime < 3.0f) { g = chroma; b = x; }
    else if (3.0f <= h_prime && h_prime < 4.0f) { g = x; b = chroma; }
    else if (4.0f <= h_prime && h_prime < 5.0f) { r = x; b = chroma; }
    else { r = chroma; b = x; }

    const float m = value - chroma;
    auto to_channel = [m](float c) {
        c = std::clamp(c + m, 0.0f, 1.0f);
        return static_cast<Uint8>(std::lround(c * 255.0f));
};
    return SDL_Color{to_channel(r), to_channel(g), to_channel(b), 230};
}

SDL_Color color_for_root_key(const std::string& key) {

    auto mix64 = [](uint64_t x) {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x = x ^ (x >> 31);
        return x;
};

    uint64_t h = static_cast<uint64_t>(std::hash<std::string>{}(key));
    h = mix64(h);

    auto u01 = [&](uint64_t bits, int shift) {

        uint32_t v = static_cast<uint32_t>((bits >> shift) & 0xFFFFFFull);
        return static_cast<float>(v) / static_cast<float>(0x1000000ull);
};

    float r1 = u01(h, 0);
    float r2 = u01(h, 24);
    float r3 = u01(h, 48);

    float hue = r1 * 360.0f;

    const float kOrangeMin = 20.0f;
    const float kOrangeMax = 45.0f;
    if (hue >= kOrangeMin && hue <= kOrangeMax) {

        float span = kOrangeMax - kOrangeMin;
        hue = std::fmod(kOrangeMax + (hue - kOrangeMin) + 60.0f, 360.0f);
    }

    float saturation = 0.72f + 0.24f * r2;
    saturation = std::clamp(saturation, 0.70f, 0.96f);
    float value = 0.78f + 0.18f * r3;
    value = std::clamp(value, 0.78f, 0.96f);

    return hsv_to_rgb(hue, saturation, value);
}

SDL_Color greyscale_of(SDL_Color c) {

    int lum = static_cast<int>(std::lround(0.299f * c.r + 0.587f * c.g + 0.114f * c.b));
    lum = std::clamp(lum, 0, 255);
    return SDL_Color{static_cast<Uint8>(lum), static_cast<Uint8>(lum), static_cast<Uint8>(lum), c.a};
}

SDL_Color mix_color(SDL_Color a, SDL_Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](Uint8 x, Uint8 y) { return static_cast<Uint8>(std::lround((1.0f - t) * x + t * y)); };
    return SDL_Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

SDL_Color grey_variant_for_level(SDL_Color root, int level) {
    if (level <= 0) return root;

    float t = 0.35f + 0.10f * static_cast<float>(level - 1);
    t = std::clamp(t, 0.0f, 0.6f);
    return mix_color(root, greyscale_of(root), t);
}

animation_editor::AnimationListPanel::ExternalRowsFingerprint fingerprint_for_external_rows(
    const std::optional<std::vector<animation_editor::AnimationListPanel::ExternalRow>>& rows) {
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    animation_editor::AnimationListPanel::ExternalRowsFingerprint fingerprint{};
    fingerprint.count = rows ? rows->size() : 0;
    std::uint64_t hash = kOffset;
    auto hash_combine = [&](std::uint64_t value) {
        hash ^= value;
        hash *= kPrime;
    };

    if (rows) {
        for (const auto& row : *rows) {
            hash_combine(static_cast<std::uint64_t>(std::hash<std::string>{}(row.id)));
            hash_combine(static_cast<std::uint64_t>(row.level));
            hash_combine(static_cast<std::uint64_t>(row.selectable ? 1u : 0u));
            hash_combine(static_cast<std::uint64_t>(row.missing_source ? 1u : 0u));
        }
    }

    fingerprint.rolling_hash = hash;
    return fingerprint;
}

}

namespace animation_editor {

AnimationListPanel::AnimationListPanel() {
    scroll_controller_.set_step_pixels(DMButton::height() + DMSpacing::section_gap());
}

void AnimationListPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    document_ = std::move(document);
    external_rows_.reset();
    last_external_rows_fingerprint_.reset();
    last_document_revision_.reset();
    rebuild_rows();
}

void AnimationListPanel::set_external_rows(std::optional<std::vector<ExternalRow>> rows) {
    const ExternalRowsFingerprint next_fingerprint = fingerprint_for_external_rows(rows);
    const bool fingerprint_unchanged = last_external_rows_fingerprint_.has_value() &&
                                       *last_external_rows_fingerprint_ == next_fingerprint;

    if (rows.has_value()) {
        if (!external_rows_.has_value()) {
            external_rows_.emplace();
        }
        auto& stored_rows = *external_rows_;
        stored_rows.clear();
        stored_rows.reserve(rows->size());
        for (auto& row : *rows) {
            stored_rows.push_back(std::move(row));
        }
    } else {
        external_rows_.reset();
    }
    last_external_rows_fingerprint_ = next_fingerprint;

    layout_dirty_ = true;
    hovered_row_.reset();
    hovered_delete_row_.reset();
    last_document_revision_.reset();
    if (fingerprint_unchanged) {
        ++row_update_skipped_count_;
#if ANIMATION_LIST_PANEL_ROW_UPDATE_DEBUG
        SDL_Log("[AnimationListPanel] external row update skipped (count=%zu hash=%llu skipped=%llu applied=%llu)",
                next_fingerprint.count,
                static_cast<unsigned long long>(next_fingerprint.rolling_hash),
                static_cast<unsigned long long>(row_update_skipped_count_),
                static_cast<unsigned long long>(row_update_applied_count_));
#endif
        if (selected_animation_id_) {
            auto it = std::find_if(display_rows_.begin(), display_rows_.end(), [&](const DisplayRow& row) {
                return row.id == *selected_animation_id_;
            });
            if (it == display_rows_.end()) {
                selected_animation_id_.reset();
                if (on_selection_changed_) {
                    on_selection_changed_(std::nullopt);
                }
            }
        }
        return;
    }

    ++row_update_applied_count_;
#if ANIMATION_LIST_PANEL_ROW_UPDATE_DEBUG
    SDL_Log("[AnimationListPanel] external row update applied (count=%zu hash=%llu skipped=%llu applied=%llu)",
            next_fingerprint.count,
            static_cast<unsigned long long>(next_fingerprint.rolling_hash),
            static_cast<unsigned long long>(row_update_skipped_count_),
            static_cast<unsigned long long>(row_update_applied_count_));
#endif
    rebuild_rows();
}

void AnimationListPanel::set_show_delete_button(bool show) {
    if (show_delete_button_ == show) {
        return;
    }
    show_delete_button_ = show;
    layout_dirty_ = true;
}

void AnimationListPanel::set_bounds(const SDL_Rect& bounds) {
    if (bounds_.x == bounds.x && bounds_.y == bounds.y &&
        bounds_.w == bounds.w && bounds_.h == bounds.h) {
        return;
    }
    bounds_ = bounds;
    scroll_controller_.set_bounds(bounds_);
    layout_dirty_ = true;
}

void AnimationListPanel::set_preview_provider(std::shared_ptr<PreviewProvider> provider) {
    preview_provider_ = std::move(provider);
}

void AnimationListPanel::set_selected_animation_id(const std::optional<std::string>& animation_id) {
    selected_animation_id_ = animation_id;
    if (layout_dirty_) {
        layout_rows();
    }
    scroll_selection_into_view();
}

bool AnimationListPanel::is_point_inside(int x, int y) const {
    SDL_Point p{x, y};
    return SDL_PointInRect(&p, &bounds_) != 0;
}

void AnimationListPanel::set_on_selection_changed(
    std::function<void(const std::optional<std::string>&)> callback) {
    on_selection_changed_ = std::move(callback);
}

void AnimationListPanel::set_on_context_menu(
    std::function<void(const std::optional<std::string>&, const SDL_Point&)> callback) {
    on_context_menu_ = std::move(callback);
}

void AnimationListPanel::set_on_delete_animation(std::function<void(const std::string&)> callback) {
    on_delete_animation_ = std::move(callback);
}

void AnimationListPanel::update() {
    rebuild_rows();
    if (layout_dirty_) {
        layout_rows();
    }
}

void AnimationListPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }

    ensure_layout();

    ClipRectGuard clip_guard(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    ui::draw_panel_background(renderer, bounds_);

    SDL_Rect clip = bounds_;
    const int inset = DMStyles::BevelDepth();
    clip.x += inset;
    clip.y += inset;
    clip.w = std::max(0, clip.w - inset * 2);
    clip.h = std::max(0, clip.h - inset * 2);
    if (clip.w > 0 && clip.h > 0) {
        SDL_SetRenderClipRect(renderer, &clip);
    }

    const DMButtonStyle& list_style = DMStyles::ListButton();
    const DMButtonStyle& accent_style = DMStyles::AccentButton();
    const DMButtonStyle& warn_style = DMStyles::WarnButton();
    const DMButtonStyle& create_style = DMStyles::CreateButton();
    const int row_padding = DMSpacing::small_gap() + 2;

    SDL_Rect sticky_header{clip.x, clip.y, clip.w, std::min(kStickyHeaderHeight, clip.h)};
    dm_draw::DrawBeveledRect(renderer,
                             sticky_header,
                             DMStyles::CornerRadius(),
                             1,
                             DMStyles::PanelHeader(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, sticky_header, DMStyles::CornerRadius(), 1, DMStyles::Border());

    std::string selected_name = selected_animation_id_.value_or("None");
    std::string asset_name = document_ ? document_->manifest_asset_key_debug() : std::string{};
    if (asset_name.empty()) {
        asset_name = "Unknown Asset";
    }
    DMLabelStyle header_style = DMStyles::Label();
    header_style.font_size = std::max(12, header_style.font_size - 2);
    header_style.color = DMStyles::TextBox().text;
    DMFontCache::instance().draw_text(renderer, header_style, "Asset: " + asset_name, sticky_header.x + row_padding, sticky_header.y + row_padding);
    DMFontCache::instance().draw_text(renderer, header_style, "Selected: " + selected_name, sticky_header.x + row_padding, sticky_header.y + row_padding + 22);

    if (display_rows_.empty()) {
        DMLabelStyle empty_style = DMStyles::Label();
        empty_style.color = DMStyles::TextBox().label.color;
        std::string state_icon{DMIcons::Info()};
        std::string state_text = "No animations yet. Use + to create or import.";
        if (!document_) {
            state_icon = std::string{DMIcons::NavDown()};
            state_text = "Loading animations... wait for asset sync.";
        }
        const std::string line = state_icon + " " + state_text;
        SDL_Point text_size = DMFontCache::instance().measure_text(empty_style, line);
        DMFontCache::instance().draw_text(renderer,
                                          empty_style,
                                          line,
                                          bounds_.x + (bounds_.w - text_size.x) / 2,
                                          bounds_.y + std::max(sticky_header.h + DMSpacing::item_gap(), (bounds_.h - text_size.y) / 2));
    }

    for (size_t i = 0; i < display_rows_.size() && i < row_geometry_.size(); ++i) {
        SDL_Rect rect = scroll_controller_.apply(row_geometry_[i].outer);
        if (!rects_intersect(rect, bounds_)) {
            continue;
        }

        const DisplayRow& row = display_rows_[i];
        const float size_factor = size_factor_for_level(row.level);
        const bool selected = selected_animation_id_ && *selected_animation_id_ == row.id;
        const bool hovered = row.selectable && hovered_row_ && *hovered_row_ == i;

        SDL_Color base = list_style.bg;
        auto it_root = root_for_id_.find(row.id);
        if (it_root != root_for_id_.end()) {
            SDL_Color root_col = color_for_root_key(it_root->second);
            base = grey_variant_for_level(root_col, row.level);
        }
        if (!row.selectable) {
            base = mix_color(base, greyscale_of(base), 0.75f);
        }
        SDL_Color fill = hovered ? list_style.hover_bg : base;
        if (!row.selectable) {
            fill = DMStyles::ButtonBaseFill();
        }
        if (row.missing_source) {
            fill = warn_style.bg;
        }

        dm_draw::DrawBeveledRect(renderer, rect, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

        SDL_Color border_col = dm_draw::DarkenColor(base, 0.45f);
        int border_thickness = 1;
        if (selected) {
            border_thickness = 2;
            border_col = accent_style.border;
        }
        dm_draw::DrawRoundedOutline(renderer, rect, DMStyles::CornerRadius(), border_thickness, border_col);

        const RowGeometry& geometry = row_geometry_[i];
        int content_x = rect.x + geometry.content_offset_x;
        int content_y = rect.y + geometry.content_offset_y;
        SDL_Rect preview_rect{rect.x + geometry.preview_rel.x,
                              rect.y + geometry.preview_rel.y,
                              geometry.preview_rel.w,
                              geometry.preview_rel.h};

        if (preview_provider_) {
            SDL_Texture* texture = preview_provider_->get_preview_texture(renderer, row.id);
            if (texture) {
                float tex_wf = 0.0f;
                float tex_hf = 0.0f;
                SDL_GetTextureSize(texture, &tex_wf, &tex_hf);
                const int tex_w = static_cast<int>(std::lround(tex_wf));
                const int tex_h = static_cast<int>(std::lround(tex_hf));
                if (tex_w > 0 && tex_h > 0 && preview_rect.w > 0 && preview_rect.h > 0) {
                    float scale = std::min(static_cast<float>(preview_rect.w) / static_cast<float>(tex_w), static_cast<float>(preview_rect.h) / static_cast<float>(tex_h));
                    int draw_w = std::max(1, static_cast<int>(tex_w * scale));
                    int draw_h = std::max(1, static_cast<int>(tex_h * scale));
                    SDL_Rect dst{preview_rect.x + (preview_rect.w - draw_w) / 2,
                                 preview_rect.y + (preview_rect.h - draw_h) / 2, draw_w, draw_h};
                    sdl_render::Texture(renderer, texture, nullptr, &dst);
                    content_x = preview_rect.x + preview_rect.w + row_padding;
                }
            }
        }

        DMLabelStyle label_style = DMStyles::Label();
        label_style.color = list_style.label.color;
        if (!row.selectable) {
            label_style.color = DMStyles::TextBox().label.color;
        }
        if (row.missing_source) {
            label_style.color = warn_style.text;
        }
        label_style.font_size = std::max(1, static_cast<int>(std::round(label_style.font_size * size_factor)));
        const std::string status_icon = row.missing_source ? "!" : (row.selectable ? std::string(DMIcons::NavRight()) : "-");
        DMFontCache::instance().draw_text(renderer, label_style, status_icon + " " + row.id, content_x, content_y + 4);

        DMLabelStyle chip_style = DMStyles::Label();
        chip_style.font_size = 12;
        chip_style.color = DMStyles::TextBox().text;
        std::vector<std::pair<std::string, SDL_Color>> chips;
        if (!row.selectable) chips.emplace_back("Inherited", DMStyles::ButtonBaseFill());
        if (row.missing_source) chips.emplace_back("Missing Source", warn_style.hover_bg);
        if (start_animation_id_ && *start_animation_id_ == row.id) chips.emplace_back("Start", create_style.bg);
        int chip_x = content_x;
        int chip_y = content_y + 28;
        for (const auto& chip : chips) {
            SDL_Point chip_sz = DMFontCache::instance().measure_text(chip_style, chip.first);
            SDL_Rect chip_rect{chip_x, chip_y, chip_sz.x + 12, chip_sz.y + 4};
            dm_draw::DrawBeveledRect(renderer, chip_rect, DMStyles::CornerRadius(), 1, chip.second, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity() * 0.4f, DMStyles::ShadowIntensity() * 0.4f);
            DMFontCache::instance().draw_text(renderer, chip_style, chip.first, chip_rect.x + 6, chip_rect.y + 2);
            chip_x += chip_rect.w + DMSpacing::small_gap();
        }

        if (show_delete_button_ && row.selectable) {
            SDL_Rect delete_rect{rect.x + geometry.delete_button_rel.x,
                                 rect.y + geometry.delete_button_rel.y,
                                 geometry.delete_button_rel.w,
                                 geometry.delete_button_rel.h};
            const DMButtonStyle& delete_style = DMStyles::DeleteButton();
            SDL_Color delete_bg = delete_style.bg;
            if (hovered_delete_row_ && *hovered_delete_row_ == i) {
                delete_bg = delete_style.hover_bg;
            }
            dm_draw::DrawBeveledRect(renderer, delete_rect, DMStyles::CornerRadius(), 1, delete_bg, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity() * 0.5f, DMStyles::ShadowIntensity() * 0.5f);
            dm_draw::DrawRoundedOutline(renderer, delete_rect, DMStyles::CornerRadius(), 1, delete_style.border);
            DMLabelStyle delete_label_style{delete_style.label.font_path, 12, delete_style.text};
            std::string delete_text{DMIcons::Close()};
            SDL_Point delete_size = DMFontCache::instance().measure_text(delete_label_style, delete_text);
            int delete_text_x = delete_rect.x + (delete_rect.w - delete_size.x) / 2;
            int delete_text_y = delete_rect.y + (delete_rect.h - delete_size.y) / 2;
            DMFontCache::instance().draw_text(renderer, delete_label_style, delete_text, delete_text_x, delete_text_y);
        }

    }

}

bool AnimationListPanel::handle_event(const SDL_Event& e) {
    ensure_layout();

    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        SDL_Point mouse{mx, my};
        const bool inside_bounds = SDL_PointInRect(&mouse, &bounds_) != 0;
        if (!inside_bounds && !DMWidgetsSliderScrollCaptured()) {
            return false;
        }
        int delta = resolve_wheel_delta(e.wheel);
        if (delta == 0) {
            return false;
        }
        bool changed = scroll_controller_.apply_wheel_delta(delta);
        if (changed) {
            hovered_row_.reset();
            hovered_delete_row_.reset();
        }
        return changed;
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = event_point(e);
        HitTestResult hit = hit_test(p);
        record_hit_result("motion", p, hit);
        if (hit.target == HitTarget::Outside) {
            hovered_row_.reset();
            hovered_delete_row_.reset();
            return false;
        }
        hovered_row_.reset();
        if (hit.row_index && *hit.row_index < display_rows_.size() && display_rows_[*hit.row_index].selectable) {
            hovered_row_ = hit.row_index;
        }
        hovered_delete_row_.reset();
        if (hit.target == HitTarget::Delete &&
            hit.row_index &&
            *hit.row_index < display_rows_.size() &&
            display_rows_[*hit.row_index].selectable) {
            hovered_delete_row_ = *hit.row_index;
        }
        return hovered_row_.has_value() || hovered_delete_row_.has_value();
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        SDL_Point p = event_point(e);
        HitTestResult hit = hit_test(p);
        record_hit_result("down", p, hit);
        if (hit.target == HitTarget::Outside) {
            return false;
        }

        if (hit.target == HitTarget::Empty || !hit.row_index) {
            if (e.button.button == SDL_BUTTON_RIGHT && on_context_menu_) {
                on_context_menu_(std::nullopt, p);
            }
            return true;
        }

        const std::string& animation_id = hit.animation_id;
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (hit.target == HitTarget::Delete) {
                const DisplayRow& row = display_rows_[*hit.row_index];
                if (row.selectable && on_delete_animation_) {
                    on_delete_animation_(animation_id);
                }
                return true;
            }
            const DisplayRow& row = display_rows_[*hit.row_index];
            if (!row.selectable) {
                return true;
            }

            selected_animation_id_ = animation_id;
            scroll_selection_into_view();
            if (on_selection_changed_) {
                on_selection_changed_(selected_animation_id_);
            }
            return true;
        }

        if (e.button.button == SDL_BUTTON_RIGHT) {
            if (on_context_menu_) {
                on_context_menu_(std::make_optional(animation_id), p);
            }
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        SDL_Point p = event_point(e);
        HitTestResult hit = hit_test(p);
        record_hit_result("up", p, hit);
        if (hit.target == HitTarget::Outside) {
            return false;
        }
        if (hit.target == HitTarget::Delete &&
            hit.row_index &&
            *hit.row_index < display_rows_.size() &&
            !display_rows_[*hit.row_index].selectable) {
            return false;
        }
        return hit.target == HitTarget::Row || hit.target == HitTarget::Delete;
    }

    return false;
}

void AnimationListPanel::rebuild_rows() {
    if (external_rows_.has_value()) {
        std::vector<DisplayRow> flattened;
        flattened.reserve(external_rows_->size());
        root_for_id_.clear();

        std::vector<std::string> root_stack;
        for (const auto& external_row : *external_rows_) {
            DisplayRow row;
            row.id = external_row.id;
            row.level = std::max(0, external_row.level);
            row.missing_source = external_row.missing_source;
            row.selectable = external_row.selectable;
            flattened.push_back(row);

            while (static_cast<int>(root_stack.size()) > row.level) {
                root_stack.pop_back();
            }
            if (row.level == 0 || root_stack.empty()) {
                root_stack.clear();
                root_stack.push_back(row.id);
                root_for_id_[row.id] = row.id;
            } else {
                root_for_id_[row.id] = root_stack.front();
                root_stack.push_back(row.id);
            }
        }

        bool changed = flattened.size() != display_rows_.size();
        if (!changed) {
            for (size_t i = 0; i < flattened.size(); ++i) {
                if (flattened[i].id != display_rows_[i].id ||
                    flattened[i].level != display_rows_[i].level ||
                    flattened[i].missing_source != display_rows_[i].missing_source ||
                    flattened[i].selectable != display_rows_[i].selectable) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            display_rows_ = std::move(flattened);
            row_geometry_.clear();
            layout_dirty_ = true;
            hovered_row_.reset();
        }
        if (selected_animation_id_) {
            auto it = std::find_if(display_rows_.begin(), display_rows_.end(), [&](const DisplayRow& row) {
                return row.id == *selected_animation_id_;
            });
            if (it == display_rows_.end()) {
                selected_animation_id_.reset();
                if (on_selection_changed_) {
                    on_selection_changed_(std::nullopt);
                }
            }
        }
        return;
    }

    if (!document_) {
        if (!display_rows_.empty()) {
            display_rows_.clear();
            row_geometry_.clear();
            content_height_ = 0;
            hovered_row_.reset();
            layout_dirty_ = true;
        }
        start_animation_id_.reset();
        last_document_revision_.reset();
        return;
    }

    const std::uint64_t document_revision = document_->revision();
    if (last_document_revision_ && *last_document_revision_ == document_revision) {
        return;
    }
    last_document_revision_ = document_revision;

    start_animation_id_ = document_->start_animation();

    auto ids = document_->animation_ids();
    std::unordered_set<std::string> id_set(ids.begin(), ids.end());

    struct NodeInfo {
        std::string id;
        std::optional<std::string> parent;
        bool missing_source = false;
        std::vector<std::string> children;
};

    std::unordered_map<std::string, NodeInfo> nodes;
    nodes.reserve(ids.size());

    for (const auto& id : ids) {
        NodeInfo node;
        node.id = id;

        bool missing_parent = false;
        std::optional<std::string> parent;

        if (auto payload_text = document_->animation_payload(id)) {
            nlohmann::json payload = nlohmann::json::parse(*payload_text, nullptr, false);
            if (payload.is_object() && payload.contains("source") && payload["source"].is_object()) {
                const nlohmann::json& source = payload["source"];
                std::string kind = source.value("kind", std::string{});
                if (kind == std::string{"animation"}) {
                    std::string candidate;
                    if (source.contains("name") && source["name"].is_string()) {
                        candidate = strings::trim_copy(source["name"].get<std::string>());
                    }
                    if (candidate.empty()) {
                        candidate = strings::trim_copy(source.value("path", std::string{}));
                    }
                    if (!candidate.empty()) {
                        if (candidate == id) {
                            missing_parent = true;
                        } else if (id_set.count(candidate) > 0) {
                            parent = candidate;
                        } else {
                            missing_parent = true;
                        }
                    }
                }
            }
        }

        node.parent = parent;
        node.missing_source = missing_parent;
        nodes.emplace(id, std::move(node));
    }

    for (auto& entry : nodes) {
        if (entry.second.parent) {
            auto it = nodes.find(*entry.second.parent);
            if (it != nodes.end()) {
                it->second.children.push_back(entry.first);
            }
        }
    }

    for (auto& entry : nodes) {
        std::sort(entry.second.children.begin(), entry.second.children.end());
    }

    std::vector<std::string> roots;
    roots.reserve(nodes.size());
    for (const auto& entry : nodes) {
        const NodeInfo& node = entry.second;
        if (!node.parent || nodes.find(*node.parent) == nodes.end()) {
            roots.push_back(entry.first);
        }
    }
    std::sort(roots.begin(), roots.end());

    std::vector<DisplayRow> flattened;
    flattened.reserve(nodes.size());

    std::unordered_set<std::string> visited;
    visited.reserve(nodes.size());

    root_for_id_.clear();

    std::function<void(const std::string&, int, const std::string&)> visit = [&](const std::string& id, int level, const std::string& root_id) {
        if (visited.count(id) != 0) {
            return;
        }
        auto it = nodes.find(id);
        if (it == nodes.end()) {
            visited.insert(id);
            return;
        }
        visited.insert(id);
        const NodeInfo& info = it->second;
        root_for_id_[id] = root_id;
        DisplayRow row;
        row.id = id;
        row.level = level;
        row.missing_source = (!info.parent.has_value() && info.missing_source);
        row.selectable = true;
        flattened.push_back(row);
        for (const auto& child : info.children) {
            visit(child, level + 1, root_id);
        }
};

    for (const auto& root : roots) {
        visit(root, 0, root);
    }

    for (const auto& entry : nodes) {
        if (visited.count(entry.first) == 0) {
            visit(entry.first, 0, entry.first);
        }
    }

    bool changed = flattened.size() != display_rows_.size();
    if (!changed) {
        for (size_t i = 0; i < flattened.size(); ++i) {
            if (flattened[i].id != display_rows_[i].id ||
                flattened[i].level != display_rows_[i].level ||
                flattened[i].missing_source != display_rows_[i].missing_source) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        display_rows_ = std::move(flattened);
        row_geometry_.clear();
        layout_dirty_ = true;
        hovered_row_.reset();
    }

    if (selected_animation_id_) {
        auto it = std::find_if(display_rows_.begin(), display_rows_.end(), [&](const DisplayRow& row) {
            return row.id == *selected_animation_id_;
        });
        if (it == display_rows_.end()) {
            selected_animation_id_.reset();
            if (on_selection_changed_) {
                on_selection_changed_(std::nullopt);
            }
        }
    }
}

void AnimationListPanel::layout_rows() {
    layout_dirty_ = false;

    const int padding = DMSpacing::panel_padding();
    const int gap = DMSpacing::small_gap();
    const int base_width = std::max(0, bounds_.w - padding * 2);
    const int row_padding = DMSpacing::small_gap() + 2;

    row_geometry_.clear();
    row_geometry_.reserve(display_rows_.size());

    int total_height = kStickyHeaderHeight;
    int cursor_y = bounds_.y + padding + kStickyHeaderHeight;

    for (size_t i = 0; i < display_rows_.size(); ++i) {
        const int level = display_rows_[i].level;
        const int row_height = row_height_for_level(level);
        const float width_factor = size_factor_for_level(level);
        const int row_width = std::max(1, static_cast<int>(std::round(base_width * width_factor)));
        SDL_Rect rect{bounds_.x + padding, cursor_y, row_width, row_height};

        RowGeometry geometry;
        geometry.outer = rect;
        geometry.content_offset_x = row_padding + indent_for_level(level);
        geometry.content_offset_y = row_padding;
        geometry.content_height = std::max(1, rect.h - row_padding * 2);
        const int thumb_size = geometry.content_height;
        geometry.preview_rel = SDL_Rect{geometry.content_offset_x,
                                        std::max(0, (rect.h - thumb_size) / 2), thumb_size, thumb_size};
        if (show_delete_button_) {
            const int delete_button_size = 16;
            geometry.delete_button_rel = SDL_Rect{rect.w - row_padding - delete_button_size,
                                                  row_padding,
                                                  delete_button_size,
                                                  delete_button_size};
        } else {
            geometry.delete_button_rel = SDL_Rect{0, 0, 0, 0};
        }

        row_geometry_.push_back(geometry);

        cursor_y += row_height;
        total_height += row_height;
        if (i + 1 < display_rows_.size()) {
            cursor_y += gap;
            total_height += gap;
        }
    }

    content_height_ = padding * 2 + total_height;
    scroll_controller_.set_content_height(content_height_);
    scroll_controller_.clamp();
}

void AnimationListPanel::scroll_selection_into_view() {
    if (!selected_animation_id_) {
        return;
    }
    ensure_layout();

    auto it = std::find_if(display_rows_.begin(), display_rows_.end(), [&](const DisplayRow& row) {
        return row.id == *selected_animation_id_;
    });
    if (it == display_rows_.end()) {
        return;
    }

    size_t index = static_cast<size_t>(std::distance(display_rows_.begin(), it));
    if (index >= row_geometry_.size()) {
        return;
    }

    const SDL_Rect& target = row_geometry_[index].outer;
    const int viewport_top = bounds_.y;
    const int viewport_bottom = bounds_.y + bounds_.h;
    const int current_scroll = scroll_controller_.scroll();

    const int row_top = target.y - current_scroll;
    const int row_bottom = row_top + target.h;

    if (row_top < viewport_top) {
        scroll_controller_.set_scroll(target.y - viewport_top);
    } else if (row_bottom > viewport_bottom) {
        scroll_controller_.set_scroll(target.y + target.h - viewport_bottom);
    }
}
std::optional<size_t> AnimationListPanel::row_index_at_point(const SDL_Point& p) const {
    for (size_t i = 0; i < row_geometry_.size(); ++i) {
        SDL_Rect rect = scroll_controller_.apply(row_geometry_[i].outer);
        if (SDL_PointInRect(&p, &rect)) {
            return i;
        }
    }
    return std::nullopt;
}

AnimationListPanel::HitTestResult AnimationListPanel::hit_test(const SDL_Point& p) const {
    HitTestResult result{};
    if (!SDL_PointInRect(&p, &bounds_)) {
        result.target = HitTarget::Outside;
        return result;
    }

    for (size_t i = 0; i < row_geometry_.size(); ++i) {
        SDL_Rect row_rect = scroll_controller_.apply(row_geometry_[i].outer);
        if (!SDL_PointInRect(&p, &row_rect)) {
            continue;
        }
        result.target = HitTarget::Row;
        result.row_index = i;
        result.animation_id = display_rows_.at(i).id;

        const RowGeometry& geometry = row_geometry_[i];
        SDL_Rect delete_rect{geometry.outer.x + geometry.delete_button_rel.x,
                             geometry.outer.y + geometry.delete_button_rel.y,
                             geometry.delete_button_rel.w,
                             geometry.delete_button_rel.h};
        delete_rect = scroll_controller_.apply(delete_rect);
        if (show_delete_button_ && display_rows_[i].selectable && SDL_PointInRect(&p, &delete_rect)) {
            result.target = HitTarget::Delete;
        }
        return result;
    }

    result.target = HitTarget::Empty;
    return result;
}

void AnimationListPanel::record_hit_result(const char* phase, const SDL_Point& p, const HitTestResult& hit) const {
    const char* target = "outside";
    switch (hit.target) {
        case HitTarget::Outside: target = "outside"; break;
        case HitTarget::Empty: target = "empty"; break;
        case HitTarget::Row: target = "row"; break;
        case HitTarget::Delete: target = "delete"; break;
    }

    std::ostringstream ss;
    ss << (phase ? phase : "event")
       << " target=" << target
       << " point=" << p.x << "," << p.y
       << " rows=" << display_rows_.size()
       << " bounds=" << bounds_.x << "," << bounds_.y << " " << bounds_.w << "x" << bounds_.h;
    if (hit.row_index) {
        ss << " row=" << *hit.row_index << " id=" << hit.animation_id;
    }
    last_hit_result_ = ss.str();
}

void AnimationListPanel::ensure_layout() const {
    if (!layout_dirty_) {
        return;
    }
    auto* self = const_cast<AnimationListPanel*>(this);
    self->layout_rows();
}
}

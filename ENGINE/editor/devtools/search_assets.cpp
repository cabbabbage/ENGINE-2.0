#include "search_assets.hpp"
#include "DockableCollapsible.hpp"
#include "FloatingDockableManager.hpp"
#include "FloatingPanelLayoutManager.hpp"
#include "widgets.hpp"
#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "tag_utils.hpp"
#include "utils/input.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"
#include "dev_mode_utils.hpp"
#include "assets/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/core/manifest_store.hpp"
#include <SDL3_ttf/SDL_ttf.h>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_set>
#include <cctype>
#include <algorithm>
#include <vector>
#include <cmath>
#include <string_view>

namespace {

struct TileStyle {
    SDL_Color background{};
    SDL_Color hover_overlay{};
    SDL_Color outline{};
    SDL_Color placeholder{};
    int padding = 0;
    int badge_height = 0;
    int corner_radius = 0;
    int bevel_depth = 0;
};

const TileStyle& tile_style() {
    static TileStyle style{
        dm_draw::DarkenColor(DMStyles::PanelBG(), 0.08f),
        dm_draw::LightenColor(DMStyles::HighlightColor(), 0.18f),
        DMStyles::Border(),
        DMStyles::CheckboxBaseFill(),
        std::max(10, DMSpacing::item_gap()),
        18,
        DMStyles::CornerRadius(),
        DMStyles::BevelDepth(),
    };
    return style;
}

std::string normalize_tag_value(std::string_view raw_value) {
    std::string normalized = tag_utils::normalize(raw_value);
    if (!normalized.empty() && normalized.front() == '#') {
        normalized.erase(normalized.begin());
    }
    return normalized;
}

void texture_size(SDL_Texture* tex, int& out_w, int& out_h) {
    float wf = 0.0f;
    float hf = 0.0f;
    if (tex && SDL_GetTextureSize(tex, &wf, &hf)) {
        out_w = static_cast<int>(std::lround(wf));
        out_h = static_cast<int>(std::lround(hf));
    } else {
        out_w = 0;
        out_h = 0;
    }
}

}  // namespace

class SearchAssets::ResultTileWidget : public Widget {
public:
    ResultTileWidget(SearchAssets* owner, const SearchAssets::Result& result)
        : owner_(owner), result_(result) {}

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return 200; }

    bool handle_event(const SDL_Event& e) override {
        if (!owner_) return false;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
            hovered_ = SDL_PointInRect(&p, &rect_);
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (SDL_PointInRect(&p, &rect_)) {
                pressed_ = true;
                return true;
            }
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            bool inside = SDL_PointInRect(&p, &rect_);
            bool was_pressed = pressed_;
            pressed_ = false;
            if (inside && was_pressed) {
                owner_->activate_result(result_);
                return true;
            }
        }
        return false;
    }

    void render(SDL_Renderer* r) const override {
        const TileStyle& palette = tile_style();
        if (!r) return;
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, palette.background.r, palette.background.g, palette.background.b, palette.background.a);
        sdl_render::FillRect(r, &rect_);

        const int pad = palette.padding;
        const int badge_h = result_.recommended ? palette.badge_height : 0;
        const int corner_radius = palette.corner_radius;
        const int bevel_depth = palette.bevel_depth;
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();

        if (result_.recommended) {
            SDL_Rect badge_rect{ rect_.x + pad, rect_.y + pad, 64, badge_h };
            const DMButtonStyle& badge_style = DMStyles::AccentButton();
            dm_draw::DrawBeveledRect(r, badge_rect, corner_radius, bevel_depth, badge_style.bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
            dm_draw::DrawRoundedOutline(r, badge_rect, std::min(corner_radius, badge_rect.h / 2), 1, badge_style.border);
            TTF_Font* badge_font = devmode::utils::load_font(std::max(12, badge_style.label.font_size - 2));
            if (badge_font) {
                const char* text = "#child";
                SDL_Surface* surf = ttf_util::RenderTextBlended(badge_font, text, badge_style.text);
                if (surf) {
                    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
                    SDL_DestroySurface(surf);
                    if (tex) {
                        int tw = 0;
                        int th = 0;
                        texture_size(tex, tw, th);
                        SDL_Rect dst{ badge_rect.x + (badge_rect.w - tw) / 2,
                                      badge_rect.y + (badge_rect.h - th) / 2,
                                      tw,
                                      th };
                        sdl_render::Texture(r, tex, nullptr, &dst);
                        SDL_DestroyTexture(tex);
                    }
                }
            }
        }

        SDL_Rect label_rect{ rect_.x + pad,
                             rect_.y + pad + badge_h,
                             std::max(0, rect_.w - 2 * pad),
                             22 };

        SDL_Texture* preview = owner_ ? owner_->preview_texture_for(result_) : nullptr;
        if (preview) {
            int tw = 0;
            int th = 0;
            texture_size(preview, tw, th);
            if (tw > 0 && th > 0) {
                int preview_top = label_rect.y + label_rect.h + pad;
                int preview_bottom = rect_.y + rect_.h - pad;
                SDL_Rect preview_rect{ rect_.x + pad,
                                       preview_top,
                                       std::max(0, rect_.w - 2 * pad),
                                       std::max(0, preview_bottom - preview_top) };
                float scale = 0.0f;
                if (preview_rect.w > 0 && preview_rect.h > 0) {
                    scale = std::min(preview_rect.w / static_cast<float>(tw),
                                     preview_rect.h / static_cast<float>(th));
                }
                if (scale > 0.0f) {
                    int dw = static_cast<int>(std::lround(tw * scale));
                    int dh = static_cast<int>(std::lround(th * scale));
                    SDL_Rect dst{ preview_rect.x + (preview_rect.w - dw) / 2,
                                  preview_rect.y + (preview_rect.h - dh) / 2,
                                  dw,
                                  dh };
                    sdl_render::Texture(r, preview, nullptr, &dst);
                }
            }
        } else {
            SDL_Rect preview_rect{ rect_.x + pad,
                                   label_rect.y + label_rect.h + pad,
                                   std::max(0, rect_.w - 2 * pad),
                                   std::max(0, rect_.h - (label_rect.h + 3 * pad + badge_h)) };
            SDL_SetRenderDrawColor(r, palette.placeholder.r, palette.placeholder.g, palette.placeholder.b, palette.placeholder.a);
            sdl_render::FillRect(r, &preview_rect);
            SDL_SetRenderDrawColor(r, palette.outline.r, palette.outline.g, palette.outline.b, palette.outline.a);
            sdl_render::Rect(r, &preview_rect);
        }

        if (hovered_) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
            SDL_SetRenderDrawColor(r, palette.hover_overlay.r, palette.hover_overlay.g, palette.hover_overlay.b, palette.hover_overlay.a);
            sdl_render::FillRect(r, &rect_);
        }

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        int radius = std::min(corner_radius, std::min(rect_.w, rect_.h) / 2);
        dm_draw::DrawRoundedOutline(r, rect_, radius, 1, palette.outline);

        const DMLabelStyle& label_style = DMStyles::Label();
        TTF_Font* label_font = devmode::utils::load_font(label_style.font_size > 0 ? label_style.font_size : 16);
        if (label_font && label_rect.w > 0) {
            std::string render_label = result_.label.empty() ? "(Unnamed)" : result_.label;
            int tw = 0;
            int th = 0;
            const std::string ellipsis = "...";
            if (ttf_util::GetStringSize(label_font, render_label, &tw, &th) && tw > label_rect.w) {
                std::string base = render_label;
                while (!base.empty()) {
                    base.pop_back();
                    std::string candidate = base + ellipsis;
                    if (ttf_util::GetStringSize(label_font, candidate, &tw, &th) && tw <= label_rect.w) {
                        render_label = std::move(candidate);
                        break;
                    }
                }
                if (base.empty()) {
                    render_label = ellipsis;
                }
            }
            SDL_Surface* surf = ttf_util::RenderTextBlended(label_font, render_label.c_str(), label_style.color);
            if (surf) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
                SDL_DestroySurface(surf);
                if (tex) {
                    int dw = 0;
                    int dh = 0;
                    texture_size(tex, dw, dh);
                    SDL_Rect dst{ label_rect.x,
                                  label_rect.y + std::max(0, (label_rect.h - dh) / 2),
                                  std::min(dw, label_rect.w),
                                  dh };
                    sdl_render::Texture(r, tex, nullptr, &dst);
                    SDL_DestroyTexture(tex);
                }
            }
        }
    }

private:
    SearchAssets* owner_ = nullptr;
    SearchAssets::Result result_;
    SDL_Rect rect_{0, 0, 0, 0};
    bool hovered_ = false;
    bool pressed_ = false;
};

SearchAssets::SearchAssets(devmode::core::ManifestStore* manifest_store)
    : manifest_store_(manifest_store) {
    if (!manifest_store_) {
        owned_manifest_store_ = std::make_unique<devmode::core::ManifestStore>();
        manifest_store_ = owned_manifest_store_.get();
    }
    panel_ = std::make_unique<DockableCollapsible>("Search Assets", true, 64, 64);
    panel_->set_expanded(true);
    panel_->set_visible(false);
    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    panel_->set_close_button_enabled(true);
    panel_->set_scroll_enabled(true);
    panel_->set_row_gap(DMSpacing::small_gap());
    panel_->set_col_gap(16);
    panel_->set_floating_content_width(520);
    panel_->reset_scroll();
    query_ = std::make_unique<DMTextBox>("Search", "");
    query_widget_ = std::make_unique<TextBoxWidget>(query_.get());
    panel_->set_rows({ { query_widget_.get() } });
    panel_->set_cell_width(220);
    last_known_position_ = panel_->position();
    pending_position_ = last_known_position_;
    has_pending_position_ = true;
    tag_data_version_ = tag_utils::tag_version();
}

SearchAssets::~SearchAssets() = default;

namespace {

FloatingPanelLayoutManager::PanelInfo build_panel_info_for_panel(DockableCollapsible* panel,
                                                                int fallback_width,
                                                                int fallback_height,
                                                                bool force_layout) {
    FloatingPanelLayoutManager::PanelInfo info;
    info.panel = panel;
    info.force_layout = force_layout;
    info.preferred_width = fallback_width;
    info.preferred_height = fallback_height;
    if (!panel) {
        return info;
    }
    SDL_Rect rect = panel->rect();
    if (rect.w > 0) {
        info.preferred_width = rect.w;
    }
    int resolved_height = rect.h > 0 ? rect.h : panel->height();
    if (resolved_height > 0) {
        info.preferred_height = resolved_height;
    }
    return info;
}

}

void SearchAssets::apply_position(int x, int y) {
    if (!panel_) {
        panel_ = std::make_unique<DockableCollapsible>("Search Assets", true, x, y);
        panel_->set_expanded(true);
        panel_->set_visible(false);
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        panel_->set_close_button_enabled(true);
        panel_->set_scroll_enabled(true);
        panel_->reset_scroll();
        panel_->set_row_gap(DMSpacing::small_gap());
        panel_->set_col_gap(16);
        panel_->set_cell_width(220);
        panel_->set_floating_content_width(520);
        if (!query_) {
            query_ = std::make_unique<DMTextBox>("Search", "");
            query_widget_ = std::make_unique<TextBoxWidget>(query_.get());
            panel_->set_rows({ { query_widget_.get() } });
        }
    }
    if (embedded_) {
        panel_->set_rect(SDL_Rect{x, y, panel_->rect().w, panel_->rect().h});
        return;
    }
    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
}

void SearchAssets::set_position(int x, int y) {
    if (embedded_) {
        embedded_rect_.x = x;
        embedded_rect_.y = y;
        if (panel_) {
            SDL_Rect rect = panel_->rect();
            rect.x = x;
            rect.y = y;
            panel_->set_rect(rect);
        }
        return;
    }
    pending_position_ = SDL_Point{x, y};
    has_pending_position_ = true;
    has_custom_position_ = false;
    apply_position(x, y);
    ensure_visible_position();
    if (panel_) {
        last_known_position_ = panel_->position();
    }
}

void SearchAssets::set_anchor_position(int x, int y) {
    if (embedded_) {
        set_position(x, y);
        return;
    }
    pending_position_ = SDL_Point{x, y};
    has_pending_position_ = true;
    if (has_custom_position_) {
        return;
    }
    apply_position(x, y);
    ensure_visible_position();
    if (panel_) {
        last_known_position_ = panel_->position();
    }
}

void SearchAssets::set_screen_dimensions(int width, int height) {
    if (width > 0) {
        screen_w_ = width;
    }
    if (height > 0) {
        screen_h_ = height;
    }
    if (embedded_) {
        if (panel_) {
            panel_->set_work_area(SDL_Rect{0, 0, embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_,
                                           embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_});
            if (embedded_rect_.w > 0 || embedded_rect_.h > 0) {
                SDL_Rect rect = embedded_rect_;
                if (rect.w <= 0) rect.w = panel_->rect().w;
                if (rect.h <= 0) rect.h = panel_->rect().h;
                panel_->set_rect(rect);
            }
        }
        return;
    }
    if (panel_) {
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        ensure_visible_position();
        last_known_position_ = panel_->position();
        if (!has_custom_position_) {
            pending_position_ = last_known_position_;
            has_pending_position_ = true;
        }
    }
}

void SearchAssets::layout_with_parent(const FloatingPanelLayoutManager::SlidingParentInfo& parent) {
    if (embedded_) {
        return;
    }
    has_custom_position_ = false;
    ensure_visible_position(&parent);
}

void SearchAssets::set_floating_stack_key(std::string key) {
    floating_stack_key_ = std::move(key);
}

void SearchAssets::set_embedded_mode(bool embedded) {
    embedded_ = embedded;
    if (!panel_) {
        return;
    }
    panel_->set_floatable(!embedded_);
    panel_->set_show_header(!embedded_);
    panel_->set_close_button_enabled(!embedded_);
    if (embedded_) {
        panel_->set_scroll_enabled(true);
        panel_->set_work_area(SDL_Rect{0, 0, embedded_rect_.w, embedded_rect_.h});
    } else {
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    }
}

void SearchAssets::set_embedded_rect(const SDL_Rect& rect) {
    embedded_rect_ = rect;
    if (!panel_) {
        return;
    }
    if (!embedded_) {
        apply_position(rect.x, rect.y);
        return;
    }
    SDL_Rect applied = rect;
    if (applied.w <= 0) {
        applied.w = panel_->rect().w > 0 ? panel_->rect().w : 260;
    }
    if (applied.h <= 0) {
        applied.h = panel_->rect().h > 0 ? panel_->rect().h : 0;
    }
    panel_->set_cell_width(std::max(120, applied.w - 20));
    if (applied.h > 0) {
        panel_->set_visible_height(applied.h);
        panel_->set_available_height_override(applied.h);
    }
    panel_->set_work_area(SDL_Rect{0, 0, applied.w, applied.h});
    panel_->set_rect(applied);
    Input dummy;
    panel_->update(dummy, applied.w, applied.h);
}

SDL_Rect SearchAssets::rect() const {
    if (!panel_) {
        return SDL_Rect{0, 0, 0, 0};
    }
    return panel_->rect();
}

std::string SearchAssets::to_lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void SearchAssets::open(Callback cb) {
    cb_ = std::move(cb);
    load_assets();  // Always reload assets when opening to ensure we have the latest from manifest store
    if (embedded_) {
        if (panel_) {
            panel_->set_visible(true);
            panel_->set_expanded(true);
            panel_->reset_scroll();
            panel_->force_pointer_ready();
            SDL_Rect applied = embedded_rect_;
            if (applied.w <= 0) applied.w = panel_->rect().w;
            if (applied.h <= 0) applied.h = panel_->rect().h;
            panel_->set_rect(applied);
            Input dummy;
            panel_->update(dummy, applied.w, applied.h);
        }
        last_query_.clear();
        filter_assets();
        return;
    }
    SDL_Point target = last_known_position_;
    if (has_pending_position_ && !has_custom_position_) {
        target = pending_position_;
    }
    apply_position(target.x, target.y);
    ensure_visible_position();
    if (!floating_stack_key_.empty()) {
        FloatingDockableManager::instance().open_floating(
            "Search Assets",
            panel_.get(),
            [this]() { this->close(); },
            floating_stack_key_);
    }
    panel_->set_visible(true);
    panel_->set_expanded(true);
    panel_->reset_scroll();
    Input dummy;
    panel_->update(dummy, screen_w_, screen_h_);
    ensure_visible_position();
    last_known_position_ = panel_->position();
    if (!has_custom_position_) {
        pending_position_ = last_known_position_;
        has_pending_position_ = true;
    }
    last_query_.clear();
    filter_assets();
}

void SearchAssets::close() {
    if (panel_) {
        if (embedded_) {
            panel_->set_visible(false);
        } else {
            last_known_position_ = panel_->position();
            if (!has_custom_position_) {
                pending_position_ = last_known_position_;
                has_pending_position_ = true;
            }
            panel_->set_visible(false);
        }
    }
    cb_ = nullptr;
}

bool SearchAssets::visible() const {
    return panel_ && panel_->is_visible();
}

void SearchAssets::set_extra_results_provider(ExtraResultsProvider provider) {
    extra_results_provider_ = std::move(provider);
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::set_asset_filter(AssetFilter filter) {
    asset_filter_ = std::move(filter);
    load_assets();
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::load_assets() {
    all_.clear();
    preview_cache_.clear();

    // Primary source: the AssetLibrary contains all loaded game assets.
    // The manifest store's "assets" section is empty at runtime (assets live in cache bundles),
    // so we must read from the library directly.
    if (assets_) {
        const auto& lib_assets = assets_->library().all();
        all_.reserve(lib_assets.size());
        for (const auto& [lib_name, info] : lib_assets) {
            if (!info) {
                continue;
            }
            Asset asset;
            asset.name = info->name.empty() ? lib_name : info->name;
            asset.manifest_name = lib_name;
            asset.tags = info->tags;
            asset.payload = nullptr;
            asset.has_child_tag = false;
            for (const auto& tag : info->tags) {
                if (normalize_tag_value(tag) == "child") {
                    asset.has_child_tag = true;
                    break;
                }
            }
            all_.push_back(std::move(asset));
        }
        return;
    }

    // Fallback: read from manifest store if no Assets pointer is available.
    if (!manifest_store_) {
        return;
    }
    auto manifest_assets = manifest_store_->assets();
    for (const auto& asset_view : manifest_assets) {
        if (!asset_view) {
            continue;
        }
        if (asset_filter_ && (!asset_view.data || !asset_filter_(*asset_view.data))) {
            continue;
        }
        Asset asset;
        const nlohmann::json& data = *asset_view.data;
        asset.name = data.value("asset_name", asset_view.name);
        if (asset.name.empty()) {
            asset.name = asset_view.name;
        }
        asset.manifest_name = asset_view.name;
        if (data.contains("tags") && data["tags"].is_array()) {
            for (const auto& tag : data["tags"]) {
                if (tag.is_string()) {
                    std::string tag_value = tag.get<std::string>();
                    asset.tags.push_back(tag_value);
                    if (normalize_tag_value(tag_value) == "child") {
                        asset.has_child_tag = true;
                    }
                }
            }
        }
        asset.payload = asset_view.data;
        all_.push_back(std::move(asset));
    }
}

void SearchAssets::filter_assets() {
    if (!panel_ || !panel_->is_visible()) return;
    auto current_version = tag_utils::tag_version();
    if (current_version != tag_data_version_) {
        load_assets();
        tag_data_version_ = current_version;
    }
    std::string q = to_lower(query_ ? query_->value() : "");
    results_.clear();
    std::unordered_set<std::string> seen_labels;
    std::set<std::string> tagset;
    std::vector<Result> asset_results;

    auto matches_query = [&](const std::string& candidate) -> bool {
        if (q.empty()) return true;
        return candidate.find(q) != std::string::npos;
    };

    for (const auto& a : all_) {
        std::string ln = to_lower(a.name);
        bool matches = matches_query(ln);
        if (!matches) {
            for (const auto& t : a.tags) {
                if (matches_query(to_lower(t))) {
                    matches = true;
                    break;
                }
            }
        }
        if (!matches) {
            continue;
        }
        Result res;
        res.label = a.name;
        res.value = a.name;
        res.manifest_name = a.manifest_name;
        res.tags = a.tags;
        res.is_tag = false;
        res.recommended = a.has_child_tag;
        if (seen_labels.insert(res.label).second) {
            asset_results.push_back(std::move(res));
        }
        for (const auto& t : a.tags) {
            if (matches_query(to_lower(t))) {
                tagset.insert(t);
            }
        }
    }

    std::sort(asset_results.begin(), asset_results.end(), [](const Result& a, const Result& b) {
        if (a.recommended != b.recommended) return a.recommended && !b.recommended;
        return to_lower(a.label) < to_lower(b.label);
    });

    if (extra_results_provider_) {
        try {
            auto extras = extra_results_provider_();
            for (auto& extra : extras) {
                if (extra.label.empty() || extra.value.empty()) {
                    continue;
                }
                std::string lowered_label = to_lower(extra.label);
                std::string lowered_value = to_lower(extra.value);
                if (!q.empty()) {
                    if (lowered_label.find(q) == std::string::npos &&
                        lowered_value.find(q) == std::string::npos) {
                        continue;
                    }
                }
                if (!seen_labels.insert(extra.label).second) {
                    continue;
                }
                asset_results.push_back(std::move(extra));
            }
        } catch (...) {
        }
    }

    results_.reserve(asset_results.size() + tagset.size());
    for (auto& res : asset_results) {
        results_.push_back(std::move(res));
    }
    for (const auto& t : tagset) {
        Result res;
        res.label = std::string("#") + t;
        res.value = t;
        res.is_tag = true;
        if (seen_labels.insert(res.label).second) {
            results_.push_back(std::move(res));
        }
    }

    rebuild_tiles();
    rebuild_rows();

    Input dummy;
    if (embedded_) {
        int w = embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_;
        int h = embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_;
        panel_->update(dummy, w, h);
    } else {
        panel_->update(dummy, screen_w_, screen_h_);
    }
}

void SearchAssets::rebuild_tiles() {
    tiles_.clear();
    tiles_.reserve(results_.size());
    for (const auto& res : results_) {
        tiles_.push_back(std::make_unique<ResultTileWidget>(this, res));
    }
}

void SearchAssets::rebuild_rows() {
    if (!panel_) return;
    DockableCollapsible::Rows rows;
    if (query_widget_) {
        rows.push_back({ query_widget_.get() });
    }
    constexpr std::size_t kPerRow = 2;
    DockableCollapsible::Row current;
    current.reserve(kPerRow);
    for (auto& tile : tiles_) {
        current.push_back(tile.get());
        if (current.size() == kPerRow) {
            rows.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        rows.push_back(current);
    }
    panel_->set_rows(rows);
}

void SearchAssets::activate_result(const Result& result) {
    std::string v = result.value;
    if (result.is_tag && (v.empty() || v.front() != '#')) {
        v.insert(v.begin(), '#');
    }
    if (cb_) cb_(v);
    close();
}

bool SearchAssets::handle_event(const SDL_Event& e) {
    if (!panel_ || !panel_->is_visible()) return false;
    SDL_Point before = panel_->position();
    bool used = panel_->handle_event(e);
    SDL_Point after = panel_->position();
    if (!embedded_) {
        if (after.x != before.x || after.y != before.y) {
            has_custom_position_ = true;
            last_known_position_ = after;
            ensure_visible_position();
        }
    }
    std::string q = query_ ? query_->value() : "";
    if (q != last_query_) { last_query_ = q; filter_assets(); }
    return used;
}

void SearchAssets::update(const Input& input) {
    if (panel_ && panel_->is_visible()) {
        if (embedded_) {
            int w = embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_;
            int h = embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_;
            panel_->update(input, w, h);
        } else {
            panel_->update(input, screen_w_, screen_h_);
            last_known_position_ = panel_->position();
            if (!has_custom_position_) {
                pending_position_ = last_known_position_;
                has_pending_position_ = true;
            }
        }
        if (tag_utils::tag_version() != tag_data_version_) {
            filter_assets();
        }
    }
}

void SearchAssets::render(SDL_Renderer* r) const {
    if (panel_ && panel_->is_visible()) panel_->render(r);
}

bool SearchAssets::is_point_inside(int x, int y) const {
    if (!panel_ || !panel_->is_visible()) return false;
    return panel_->is_point_inside(x, y);
}

void SearchAssets::set_manifest_store(devmode::core::ManifestStore* manifest_store) {
    if (manifest_store == manifest_store_) {
        return;
    }
    manifest_store_ = manifest_store;
    if (!manifest_store_) {
        owned_manifest_store_ = std::make_unique<devmode::core::ManifestStore>();
        manifest_store_ = owned_manifest_store_.get();
    } else {
        owned_manifest_store_.reset();
    }
    all_.clear();
    results_.clear();
    preview_cache_.clear();
    tag_data_version_ = 0;
    load_assets();
}

void SearchAssets::set_assets(Assets* assets) {
    if (assets == assets_) {
        return;
    }
    assets_ = assets;
    all_.clear();
    results_.clear();
    preview_cache_.clear();
    tag_data_version_ = 0;
    load_assets();
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::set_query_for_testing(const std::string& value) {
    if (query_) {
        query_->set_value(value);
    }
    filter_assets();
}

std::vector<std::pair<std::string, bool>> SearchAssets::results_for_testing() const {
    std::vector<std::pair<std::string, bool>> out;
    out.reserve(results_.size());
    for (const auto& res : results_) {
        out.emplace_back(res.value, res.is_tag);
    }
    return out;
}

SDL_Texture* SearchAssets::default_frame_texture(const AssetInfo& info) const {
    auto find_frame = [](const AssetInfo& inf, const std::string& key) -> SDL_Texture* {
        if (key.empty()) return nullptr;
        auto it = inf.animations.find(key);
        if (it != inf.animations.end() && !it->second.frames.empty()) {
            auto& frame = it->second.frames.front();
            if (frame && !frame->variants.empty()) {
                return frame->variants[0].base_texture;
            }
        }
        return nullptr;
    };

    if (SDL_Texture* tex = find_frame(info, "default")) {
        return tex;
    }
    if (SDL_Texture* tex = find_frame(info, info.start_animation)) {
        return tex;
    }
    if (SDL_Texture* tex = find_frame(info, "start")) {
        return tex;
    }
    for (const auto& kv : info.animations) {
        if (!kv.second.frames.empty() && !kv.second.frames.front()->variants.empty()) {
            return kv.second.frames.front()->variants[0].base_texture;
        }
    }
    return nullptr;
}

SDL_Texture* SearchAssets::preview_texture_for(const Result& result) const {
    if (result.is_tag || !assets_) {
        return nullptr;
    }
    const std::string key = !result.manifest_name.empty() ? result.manifest_name : result.value;
    auto it = preview_cache_.find(key);
    if (it != preview_cache_.end()) {
        return it->second;
    }

    SDL_Texture* tex = nullptr;
    AssetLibrary& lib = assets_->library();
    auto info = lib.get(key);
    if (!info && key != result.value) {
        info = lib.get(result.value);
    }
    if (info) {
        if (SDL_Renderer* renderer = assets_->renderer()) {
            std::unordered_set<std::string> names{key};
            lib.loadAnimationsFor(renderer, names);
        }
        tex = default_frame_texture(*info);
    }
    preview_cache_[key] = tex;
    return tex;
}

FloatingPanelLayoutManager::PanelInfo SearchAssets::build_panel_info(bool force_layout) const {
    constexpr int kFallbackWidth = 520;
    constexpr int kFallbackHeight = 400;
    return build_panel_info_for_panel(panel_.get(), kFallbackWidth, kFallbackHeight, force_layout);
}

void SearchAssets::ensure_visible_position(const FloatingPanelLayoutManager::SlidingParentInfo* parent) {
    if (embedded_) {
        return;
    }
    if (!panel_) {
        return;
    }
    if (has_custom_position_) {
        return;
    }

    FloatingPanelLayoutManager::PanelInfo info = build_panel_info(true);

    if (parent) {
        SDL_Point placement = FloatingPanelLayoutManager::instance().positionFor(info, parent);
        panel_->set_position_from_layout_manager(placement.x, placement.y);
    } else {
        std::vector<FloatingPanelLayoutManager::PanelInfo> panels;
        panels.push_back(info);
        FloatingPanelLayoutManager::instance().layoutAll(panels);
    }

    last_known_position_ = panel_->position();
    pending_position_ = last_known_position_;
    has_pending_position_ = true;
}

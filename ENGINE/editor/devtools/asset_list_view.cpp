#include "asset_list_view.hpp"

#include "widgets.hpp"
#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "utils/string_utils.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"
#include "dev_mode_utils.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <utility>

namespace {
const SDL_Color kTileBG  = dm::rgba(24, 36, 56, 210);
const SDL_Color kTileHL  = dm::rgba(59, 130, 246, 110);
const SDL_Color kTileBd  = DMStyles::Border();
const SDL_Color kPlaceholder = DMStyles::CheckboxBaseFill();

using vibble::strings::to_lower_copy;

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

class AssetListView::AssetTileWidget : public Widget {
public:
    AssetTileWidget(AssetListView* owner,
                    Entry entry,
                    Callbacks callbacks,
                    bool enable_multi_select,
                    bool initially_selected,
                    bool show_delete)
        : owner_(owner),
          entry_(std::move(entry)),
          callbacks_(std::move(callbacks)),
          multi_select_enabled_(enable_multi_select),
          multi_select_selected_(initially_selected),
          show_delete_button_(show_delete) {
        set_rect(rect_);
    }

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        delete_rect_ = SDL_Rect{ rect_.x + kPad, rect_.y + kPad, kDeleteButtonSize, kDeleteButtonSize };
    }

    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return 200; }

    bool handle_event(const SDL_Event& e) override {
        if (multi_select_enabled_) {
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
                hovered_ = SDL_PointInRect(&p, &rect_);
                delete_hovered_ = SDL_PointInRect(&p, &delete_rect_);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
                if (SDL_PointInRect(&p, &rect_)) {
                    multi_select_pressed_ = true;
                    return true;
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
                bool inside = SDL_PointInRect(&p, &rect_);
                bool was_pressed = multi_select_pressed_;
                multi_select_pressed_ = false;
                if (inside && was_pressed) {
                    multi_select_selected_ = !multi_select_selected_;
                    if (callbacks_.on_multi_select_toggle) {
                        callbacks_.on_multi_select_toggle(entry_, multi_select_selected_);
                    }
                    return true;
                }
            }
            return false;
        }

        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
            hovered_ = SDL_PointInRect(&p, &rect_);
            delete_hovered_ = SDL_PointInRect(&p, &delete_rect_);
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (!SDL_PointInRect(&p, &rect_)) {
                return false;
            }
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (show_delete_button_ && SDL_PointInRect(&p, &delete_rect_)) {
                    delete_pressed_ = true;
                    return true;
                }
                pressed_ = true;
                return true;
            }
            if (e.button.button == SDL_BUTTON_RIGHT) {
                if (show_delete_button_ && SDL_PointInRect(&p, &delete_rect_)) {
                    return true;
                }
                right_pressed_ = true;
                return true;
            }
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            if (e.button.button == SDL_BUTTON_LEFT) {
                bool inside_delete = SDL_PointInRect(&p, &delete_rect_);
                bool inside_tile = SDL_PointInRect(&p, &rect_);
                bool was_delete = delete_pressed_;
                bool was_tile = pressed_;
                delete_pressed_ = false;
                pressed_ = false;
                if (inside_delete && was_delete) {
                    if (callbacks_.on_delete) callbacks_.on_delete(entry_);
                    return true;
                }
                if (inside_tile && was_tile) {
                    if (callbacks_.on_click) callbacks_.on_click(entry_);
                    return true;
                }
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                bool was = right_pressed_;
                right_pressed_ = false;
                if (was && SDL_PointInRect(&p, &rect_)) {
                    if (callbacks_.on_right_click) callbacks_.on_right_click(entry_);
                    return true;
                }
            }
        }
        return false;
    }

    void render(SDL_Renderer* r) const override {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, kTileBG.r, kTileBG.g, kTileBG.b, kTileBG.a);
        sdl_render::FillRect(r, &rect_);

        const int pad = kPad;
        const int label_h = 24;

        SDL_Rect button_rect = delete_rect_;
        const int corner_radius = DMStyles::CornerRadius();
        const int bevel_depth = DMStyles::BevelDepth();
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();

        if (multi_select_enabled_) {
            SDL_Color checkbox_bg = multi_select_selected_ ? DMStyles::CheckboxHoverFill() : DMStyles::CheckboxBaseFill();
            if (delete_hovered_) {
                checkbox_bg = DMStyles::CheckboxHoverFill();
            }
            dm_draw::DrawBeveledRect( r, button_rect, corner_radius, bevel_depth, checkbox_bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
            SDL_Color border = multi_select_selected_ ? DMStyles::CheckboxActiveOutline() : DMStyles::CheckboxOutlineColor();
            if (delete_hovered_) {
                border = DMStyles::CheckboxHoverOutline();
            }
            dm_draw::DrawRoundedOutline( r, button_rect, std::min(corner_radius, button_rect.w / 2), 1, border);
            if (multi_select_selected_) {
                SDL_Color check = DMStyles::CheckboxCheckColor();
                SDL_SetRenderDrawColor(r, check.r, check.g, check.b, check.a);
                const int inset = std::max(3, button_rect.w / 5);
                SDL_RenderLine(r, button_rect.x + inset, button_rect.y + button_rect.h / 2, button_rect.x + button_rect.w / 2, button_rect.y + button_rect.h - inset + 1);
                SDL_RenderLine(r, button_rect.x + button_rect.w / 2, button_rect.y + button_rect.h - inset + 1, button_rect.x + button_rect.w - inset, button_rect.y + inset);
            }
        } else if (show_delete_button_) {
            const auto& delete_style = DMStyles::DeleteButton();
            SDL_Color delete_bg = delete_style.bg;
            if (delete_pressed_) {
                delete_bg = delete_style.press_bg;
            } else if (delete_hovered_) {
                delete_bg = delete_style.hover_bg;
            }
            dm_draw::DrawBeveledRect( r, button_rect, corner_radius, bevel_depth, delete_bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
            dm_draw::DrawRoundedOutline( r, button_rect, corner_radius, 1, delete_style.border);
            SDL_SetRenderDrawColor(r, delete_style.text.r, delete_style.text.g, delete_style.text.b, delete_style.text.a);
            const int cross_inset = std::max(bevel_depth + 1, button_rect.w / 4);
            SDL_RenderLine(r, button_rect.x + cross_inset, button_rect.y + cross_inset, button_rect.x + button_rect.w - cross_inset, button_rect.y + button_rect.h - cross_inset);
            SDL_RenderLine(r, button_rect.x + button_rect.w - cross_inset, button_rect.y + cross_inset, button_rect.x + cross_inset, button_rect.y + button_rect.h - cross_inset);
        }

        int label_left = button_rect.x + button_rect.w + pad;
        int label_right = rect_.x + rect_.w - pad;
        if (label_left > label_right) {
            label_left = rect_.x + pad;
        }
        SDL_Rect label_rect{ label_left, rect_.y + pad, std::max(0, label_right - label_left), label_h };

        std::string label_text = entry_.label.empty() ? "(Unnamed)" : entry_.label;
        TTF_Font* label_font = devmode::utils::load_font(15);
        std::string render_label = label_text;
        if (label_font && label_rect.w > 0) {
            int tw = 0;
            int th = 0;
            const std::string ellipsis = "...";
            if (ttf_util::GetStringSize(label_font, render_label, &tw, &th) && tw > label_rect.w) {
                std::string base = label_text;
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
        }

        SDL_Texture* tex = owner_ ? owner_->preview_texture_for(entry_) : nullptr;
        if (tex) {
            int tw = 0;
            int th = 0;
            texture_size(tex, tw, th);
            if (tw > 0 && th > 0) {
                SDL_Rect image_rect{ rect_.x + pad,
                                     label_rect.y + label_rect.h + pad,
                                     rect_.w - 2 * pad,
                                     rect_.h - (label_rect.h + 3 * pad) };
                image_rect.h = std::max(image_rect.h, 0);
                if (image_rect.w > 0 && image_rect.h > 0) {
                    float scale = std::min(image_rect.w / static_cast<float>(tw),
                                           image_rect.h / static_cast<float>(th));
                    if (scale > 0.0f) {
                        int dw = static_cast<int>(std::lround(tw * scale));
                        int dh = static_cast<int>(std::lround(th * scale));
                        SDL_Rect dst{ image_rect.x + (image_rect.w - dw) / 2,
                                      image_rect.y + (image_rect.h - dh) / 2,
                                      dw, dh };
                        sdl_render::Texture(r, tex, nullptr, &dst);
                    }
                }
            }
        } else {
            SDL_Rect preview_rect{ rect_.x + pad,
                                   label_rect.y + label_rect.h + pad,
                                   std::max(0, rect_.w - 2 * pad),
                                   std::max(0, rect_.h - (label_rect.h + 3 * pad)) };
            SDL_SetRenderDrawColor(r, kPlaceholder.r, kPlaceholder.g, kPlaceholder.b, kPlaceholder.a);
            sdl_render::FillRect(r, &preview_rect);
            SDL_SetRenderDrawColor(r, kTileBd.r, kTileBd.g, kTileBd.b, kTileBd.a);
            sdl_render::Rect(r, &preview_rect);
        }

        if (label_font && label_rect.w > 0) {
            SDL_Color text_color = DMStyles::Label().color;
            SDL_Surface* surf = ttf_util::RenderTextBlended(label_font, render_label.c_str(), text_color);
            if (surf) {
                SDL_Texture* tex_label = SDL_CreateTextureFromSurface(r, surf);
                SDL_DestroySurface(surf);
                if (tex_label) {
                    int dw = 0;
                    int dh = 0;
                    texture_size(tex_label, dw, dh);
                    if (dw > label_rect.w) {
                        dw = label_rect.w;
                    }
                    SDL_Rect dst{ label_rect.x,
                                  label_rect.y + std::max(0, (label_rect.h - dh) / 2), dw, dh };
                    sdl_render::Texture(r, tex_label, nullptr, &dst);
                    SDL_DestroyTexture(tex_label);
                }
            }
        }

        if (hovered_) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
            SDL_SetRenderDrawColor(r, kTileHL.r, kTileHL.g, kTileHL.b, kTileHL.a);
            sdl_render::FillRect(r, &rect_);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const int tile_radius = std::min(DMStyles::CornerRadius(), std::min(rect_.w, rect_.h) / 2);
        dm_draw::DrawRoundedOutline(r, rect_, tile_radius, 1, kTileBd);
    }

private:
    static constexpr int kPad = 8;
    static constexpr int kDeleteButtonSize = 24;

    AssetListView* owner_ = nullptr;
    Entry entry_;
    Callbacks callbacks_;
    SDL_Rect rect_{0,0,0,0};
    SDL_Rect delete_rect_{0,0,kDeleteButtonSize,kDeleteButtonSize};
    bool hovered_ = false;
    bool pressed_ = false;
    bool right_pressed_ = false;
    bool delete_hovered_ = false;
    bool delete_pressed_ = false;
    bool multi_select_enabled_ = false;
    bool multi_select_selected_ = false;
    bool multi_select_pressed_ = false;
    bool show_delete_button_ = false;
};

AssetListView::AssetListView() {
    search_box_ = std::make_unique<DMTextBox>("Search", "");
    search_widget_ = std::make_unique<TextBoxWidget>(search_box_.get(), true);
}

AssetListView::~AssetListView() = default;

DMTextBox* AssetListView::search_box() const {
    return search_box_.get();
}

TextBoxWidget* AssetListView::search_widget() const {
    return search_widget_.get();
}

void AssetListView::set_assets(Assets* assets) {
    if (assets == assets_) {
        return;
    }
    assets_ = assets;
    preview_cache_.clear();
}

void AssetListView::set_entries(std::vector<Entry> entries) {
    entries_ = std::move(entries);
    preview_cache_.clear();
}

void AssetListView::set_callbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void AssetListView::set_multi_select_enabled(bool enabled) {
    multi_select_enabled_ = enabled;
}

void AssetListView::set_selected_values(std::unordered_set<std::string> selected) {
    selected_values_ = std::move(selected);
}

void AssetListView::set_query(const std::string& q) {
    query_ = q;
    if (search_box_) {
        search_box_->set_value(q);
    }
}

const std::string& AssetListView::query() const {
    return query_;
}

bool AssetListView::update_query_from_widget() {
    if (!search_box_) return false;
    std::string current = search_box_->value();
    if (current != query_) {
        query_ = std::move(current);
        return true;
    }
    return false;
}

void AssetListView::refresh_tiles() {
    rebuild_tiles();
}

std::vector<Widget*> AssetListView::tile_ptrs() const {
    std::vector<Widget*> out;
    out.reserve(tiles_.size());
    for (const auto& t : tiles_) {
        out.push_back(t.get());
    }
    return out;
}

DockableCollapsible::Rows AssetListView::rows(std::size_t per_row, bool include_search_row) const {
    DockableCollapsible::Rows rows;
    append_rows(rows, per_row, include_search_row);
    return rows;
}

void AssetListView::append_rows(DockableCollapsible::Rows& rows,
                                std::size_t per_row,
                                bool include_search_row) const {
    if (include_search_row && search_widget_) {
        rows.push_back({ search_widget_.get() });
    }

    DockableCollapsible::Row current_row;
    current_row.reserve(per_row);
    for (const auto& t : tiles_) {
        current_row.push_back(t.get());
        if (current_row.size() == per_row) {
            rows.push_back(current_row);
            current_row.clear();
        }
    }
    if (!current_row.empty()) {
        rows.push_back(current_row);
    }
}

SDL_Texture* AssetListView::default_frame_texture(const AssetInfo& info) const {
    auto find_frame = [](const AssetInfo& inf, const std::string& key) -> SDL_Texture* {
        if (key.empty()) return nullptr;
        auto it = inf.animations.find(key);
        if (it != inf.animations.end() && !it->second.frames.empty()) {
            return (!it->second.frames.empty() && !it->second.frames.front()->variants.empty()) ? it->second.frames.front()->variants[0].base_texture : nullptr;
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

SDL_Texture* AssetListView::preview_texture_for(const Entry& entry) const {
    if (entry.is_tag || !assets_) {
        return nullptr;
    }

    std::string key = entry.manifest_name.empty() ? entry.value : entry.manifest_name;
    if (key.empty() && entry.info) {
        key = entry.info->name;
    }
    auto it = preview_cache_.find(key);
    if (it != preview_cache_.end()) {
        return it->second;
    }

    SDL_Texture* tex = nullptr;
    if (entry.info) {
        tex = default_frame_texture(*entry.info);
    }

    if (!tex) {
        AssetLibrary& lib = assets_->library();
        auto info = lib.get(key);
        if (!info && !entry.value.empty() && entry.value != key) {
            info = lib.get(entry.value);
        }
        if (info) {
            if (SDL_Renderer* renderer = assets_->renderer()) {
                std::unordered_set<std::string> names{key};
                lib.loadAnimationsFor(renderer, names);
            }
            tex = default_frame_texture(*info);
        }
    }

    preview_cache_[key] = tex;
    return tex;
}

bool AssetListView::matches_query(const Entry& entry, const std::string& query) {
    if (query.empty()) return true;

    std::istringstream ss(query);
    std::string token;
    std::string name_lower = to_lower_copy(entry.label);

    while (ss >> token) {
        if (token.empty()) continue;

        if (token.front() == '#') {
            std::string tag = token.substr(1);
            if (tag.empty()) continue;
            std::string needle = to_lower_copy(tag);
            bool tag_match = std::any_of(entry.tags.begin(), entry.tags.end(), [&](const std::string& t){
                return to_lower_copy(t).find(needle) != std::string::npos;
            });
            if (!tag_match) {
                return false;
            }
        } else {
            std::string needle = to_lower_copy(token);
            if (needle.empty()) continue;
            bool in_name = name_lower.find(needle) != std::string::npos;
            if (!in_name) {
                bool in_tags = std::any_of(entry.tags.begin(), entry.tags.end(), [&](const std::string& t){
                    return to_lower_copy(t).find(needle) != std::string::npos;
                });
                if (!in_tags) {
                    return false;
                }
            }
        }
    }

    return true;
}

void AssetListView::rebuild_tiles() {
    tiles_.clear();
    tiles_.reserve(entries_.size());

    bool show_delete = callbacks_.on_delete != nullptr;
    for (const auto& entry : entries_) {
        if (!matches_query(entry, query_)) continue;
        bool is_selected = selected_values_.find(entry.value) != selected_values_.end();
        tiles_.push_back(std::make_unique<AssetTileWidget>(
            this,
            entry,
            callbacks_,
            multi_select_enabled_,
            is_selected,
            show_delete && !entry.is_tag
        ));
    }
}

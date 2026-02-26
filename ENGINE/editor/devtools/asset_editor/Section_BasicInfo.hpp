#pragma once

#include "../DockableCollapsible.hpp"
#include "core/AssetsManager.hpp"
#include "assets/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render.hpp"
#include "widgets.hpp"
#include "devtools/asset_info_sections.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "dm_styles.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

class AssetInfoUI;

class Section_BasicInfo : public DockableCollapsible {
  public:
    Section_BasicInfo();
    ~Section_BasicInfo() override = default;

    void set_ui(AssetInfoUI* ui) { ui_ = ui; }

    void build() override;
    void layout() override { DockableCollapsible::layout(); }
    bool handle_event(const SDL_Event& e) override;
    void render_content(SDL_Renderer* r) const override {}
    void render_world_overlay(SDL_Renderer* r, const WarpedScreenGrid& cam, const Asset* target, float reference_screen_height) const;

  private:
    static int find_index(const std::vector<std::string>& opts, const std::string& value);

    std::unique_ptr<DMDropdown>  dd_type_;
    std::unique_ptr<DMSlider>    s_scale_pct_;
    std::unique_ptr<DMCheckbox>  c_flipable_;
    std::unique_ptr<DMCheckbox>  c_tillable_;
    std::unique_ptr<DMTextBox>   tb_starting_health_;
    std::unique_ptr<DMButton>    apply_btn_;
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::vector<std::string> type_options_;
    AssetInfoUI* ui_ = nullptr;
    void on_scale_slider_value_changed(int new_value);

  protected:
    std::string_view lock_settings_namespace() const override { return "asset_info"; }
    std::string_view lock_settings_id() const override { return "basic"; }
};

inline Section_BasicInfo::Section_BasicInfo()
    : DockableCollapsible("Basic Info", false) {}

inline int Section_BasicInfo::find_index(const std::vector<std::string>& opts, const std::string& value) {
    std::string canonical = asset_types::canonicalize(value);
    auto it = std::find(opts.begin(), opts.end(), canonical);
    if (it != opts.end()) {
        return static_cast<int>(std::distance(opts.begin(), it));
    }
    auto fallback = std::find(opts.begin(), opts.end(), std::string(asset_types::object));
    if (fallback != opts.end()) {
        return static_cast<int>(std::distance(opts.begin(), fallback));
    }
    return 0;
}

inline void Section_BasicInfo::build() {
    widgets_.clear();
    DockableCollapsible::Rows rows;
    if (!info_) {
        auto placeholder = std::make_unique<ReadOnlyTextBoxWidget>( "", "No asset selected. Select an asset from the library or scene to view and edit its information.");
        rows.push_back({ placeholder.get() });
        widgets_.push_back(std::move(placeholder));
        set_rows(rows);
        return;
    }

    type_options_ = asset_types::all_as_strings();

    const bool is_area_asset = (asset_types::canonicalize(info_->type) == std::string(asset_types::area));
    const bool is_tiled_asset = info_->tillable;
    if (is_area_asset) {
        type_options_.clear();
        type_options_.emplace_back(std::string(asset_types::area));
    } else {

        type_options_.erase(std::remove(type_options_.begin(), type_options_.end(), std::string(asset_types::area)), type_options_.end());
    }
    dd_type_ = std::make_unique<DMDropdown>("Type", type_options_, find_index(type_options_, info_->type));
    int pct = std::max(0, static_cast<int>(std::lround(info_->scale_factor * 100.0f)));
    s_scale_pct_ = std::make_unique<DMSlider>("Scale (%)", 1, 400, pct);
    s_scale_pct_->set_on_value_changed([this](int value) { this->on_scale_slider_value_changed(value); });
    if (!is_tiled_asset) {
        c_flipable_  = std::make_unique<DMCheckbox>("Flipable (can invert)", info_->flipable);
    } else {
        c_flipable_.reset();
    }
    c_tillable_ = std::make_unique<DMCheckbox>("Tileable (grid tiles)", info_->tillable);

    auto w_type = std::make_unique<DropdownWidget>(dd_type_.get());
    rows.push_back({ w_type.get() });
    widgets_.push_back(std::move(w_type));

    auto w_scale = std::make_unique<SliderWidget>(s_scale_pct_.get());
    rows.push_back({ w_scale.get() });
    widgets_.push_back(std::move(w_scale));

    tb_starting_health_ = std::make_unique<DMTextBox>("Starting Health", std::to_string(info_->starting_health));
    auto w_health = std::make_unique<TextBoxWidget>(tb_starting_health_.get());
    rows.push_back({ w_health.get() });
    widgets_.push_back(std::move(w_health));

    if (c_flipable_) {
        auto w_flip = std::make_unique<CheckboxWidget>(c_flipable_.get());
        rows.push_back({ w_flip.get() });
        widgets_.push_back(std::move(w_flip));
    }

    auto w_tillable = std::make_unique<CheckboxWidget>(c_tillable_.get());
    rows.push_back({ w_tillable.get() });
    widgets_.push_back(std::move(w_tillable));

    if (!apply_btn_) {
        apply_btn_ = std::make_unique<DMButton>("Apply Settings", &DMStyles::AccentButton(), 180, DMButton::height());
    }
    auto w_apply = std::make_unique<ButtonWidget>(apply_btn_.get(), [this]() {
        if (ui_) ui_->request_apply_section(AssetInfoSectionId::BasicInfo);
    });
    rows.push_back({ w_apply.get() });
    widgets_.push_back(std::move(w_apply));

    set_rows(rows);
}

inline bool Section_BasicInfo::handle_event(const SDL_Event& e) {
    bool used = DockableCollapsible::handle_event(e);
    if (!info_) return used;

    if (!used) {
        if (dd_type_ && dd_type_->handle_event(e)) used = true;
        if (s_scale_pct_ && s_scale_pct_->handle_event(e)) used = true;
        if (tb_starting_health_ && tb_starting_health_->handle_event(e)) used = true;
        if (c_flipable_ && c_flipable_->handle_event(e)) used = true;
        if (c_tillable_ && c_tillable_->handle_event(e)) used = true;
    }

    bool changed = false;
    bool rebuild_needed = false;
    bool tile_changed = false;
    bool render_settings_changed = false;
    bool type_changed = false;
    if (dd_type_ && !type_options_.empty()) {
        int idx = std::clamp(dd_type_->selected(), 0, static_cast<int>(type_options_.size()) - 1);
        std::string selected = asset_types::canonicalize(type_options_[idx]);
        std::string current = asset_types::canonicalize(info_->type);
        const bool is_area_asset = (current == std::string(asset_types::area));
        const bool selecting_area = (selected == std::string(asset_types::area));

        if (!(is_area_asset && !selecting_area) && !(!is_area_asset && selecting_area) && current != selected) {
            info_->set_asset_type(selected);
            changed = true;
            render_settings_changed = true;
            type_changed = true;
        }
    }

    if (tb_starting_health_ && !tb_starting_health_->value().empty()) {
        try {
            int new_health = std::stoi(tb_starting_health_->value());
            if (info_->starting_health != new_health) {
                info_->set_starting_health(new_health);
                changed = true;
            }
        } catch (const std::exception&) {
            // Invalid input, ignore
        }
    }

    if (c_flipable_ && info_->flipable != c_flipable_->value()) {
        info_->set_flipable(c_flipable_->value());
        changed = true;
        render_settings_changed = true;
    }

    if (c_tillable_ && info_->tillable != c_tillable_->value()) {
        info_->set_tillable(c_tillable_->value());
        changed = true;
        tile_changed = true;
        rebuild_needed = true;
    }

    if (changed) {
        auto on_success = [this, render_settings_changed, type_changed, tile_changed]() {
            if (!ui_) return;
            if (tile_changed) ui_->sync_target_tiling_state();
            if (render_settings_changed) ui_->sync_target_basic_render_settings(type_changed);
        };
        if (ui_) {
            ui_->enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                       "Basic info",
                                       on_success);
        } else {
            on_success();
        }
    }
    if (rebuild_needed) {
        build();
    }
    return used || changed;
}

inline void Section_BasicInfo::render_world_overlay(SDL_Renderer* r,
                                                    const WarpedScreenGrid& cam,
                                                    const Asset* target,
                                                    float reference_screen_height) const {
    if (!is_expanded() || !target || !target->info) return;
    (void)reference_screen_height;
    Assets* assets = ui_ ? ui_->assets() : nullptr;

    SDL_Texture* tex = target->get_current_frame();
    int fw = target->cached_w;
    int fh = target->cached_h;
    if ((fw == 0 || fh == 0) && tex) {
        float fwf = 0.0f;
        float fhf = 0.0f;
        if (SDL_GetTextureSize(tex, &fwf, &fhf)) {
            fw = static_cast<int>(std::lround(fwf));
            fh = static_cast<int>(std::lround(fhf));
        }
    }
    if (fw == 0 || fh == 0) {
        if (target->info) {
            fw = target->info->original_canvas_width;
            fh = target->info->original_canvas_height;
        }
    }
    if (fw == 0 || fh == 0) return;

    float package_scale = target->current_nearest_variant_scale * target->current_remaining_scale_adjustment;
    if (!std::isfinite(package_scale) || package_scale <= 0.0f) {
        package_scale = 1.0f;
    }

    int base_w = fw;
    int base_h = fh;
    if (target->info) {
        if (target->info->original_canvas_width > 0) base_w = target->info->original_canvas_width;
        if (target->info->original_canvas_height > 0) base_h = target->info->original_canvas_height;
    }

    float base_sw = static_cast<float>(base_w) * package_scale;
    float base_sh = static_cast<float>(base_h) * package_scale;
    if (base_sw <= 0.0f || base_sh <= 0.0f) return;

    SDL_FPoint screen_pos = cam.map_to_screen(target->world_point());
    float distance_scale = 1.0f;
    float vertical_scale = 1.0f;
    if (auto* gp = cam.grid_point_for_asset(target)) {
        screen_pos = gp->screen;
        distance_scale = std::max(0.0001f, gp->perspective_scale);
        vertical_scale = std::max(0.0001f, gp->vertical_scale);
    }

    float scaled_sw = base_sw * distance_scale;
    float scaled_sh = base_sh * distance_scale;
    float final_visible_h = scaled_sh * vertical_scale;

    int sw = std::max(1, static_cast<int>(std::round(scaled_sw)));
    int sh = std::max(1, static_cast<int>(std::round(final_visible_h)));
    if (sw <= 0 || sh <= 0) return;

        SDL_Point world_point{ target->world_point().x, target->world_point().y };
        float center_x = screen_pos.x;
        if (assets) {

            if (!(assets->player == target)) {

            }
        }
        const int   left     = static_cast<int>(std::lround(center_x - static_cast<float>(sw) * 0.5f));
        const int   top      = static_cast<int>(std::lround(screen_pos.y)) - sh;
    SDL_Rect bounds{ left, top, sw, sh };

}

inline void Section_BasicInfo::on_scale_slider_value_changed(int new_value) {
    if (!info_) return;
    info_->set_scale_percentage(static_cast<float>(new_value));
    if (ui_) {
        ui_->enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                   "Scale",
                                   [this]() {
                                       if (ui_) {
                                           ui_->refresh_target_asset_scale();
                                       }
                                   });
    }
}

#include "asset_info_ui.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <SDL3/SDL_log.h>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <functional>

#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "utils/input.hpp"
#include "utils/area.hpp"
#include "utils/string_utils.hpp"
#include "utils/cache_manager.hpp"
#include "widgets.hpp"
#include "tag_utils.hpp"

#include "DockableCollapsible.hpp"
#include "DockManager.hpp"
#include "SlidingWindowContainer.hpp"
#include "dm_styles.hpp"
#include "dev_mode_utils.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "gameplay/map_generation/room.hpp"
#include "core/AssetsManager.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/world/chunk.hpp"
#include "utils/map_grid_settings.hpp"
#include "devtools/core/manifest_store.hpp"
#include "asset_editor/animation_editor_window/AnimationEditorWindow.hpp"
#include "asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "core/AssetsManager.hpp"
#include "assets/asset/Asset.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render.hpp"
#include "search_assets.hpp"
#include "draw_utils.hpp"
#include <SDL3_ttf/SDL_ttf.h>
#include "devtools/config/room_config/tag_editor_widget.hpp"
#include "devtools/manifest_asset_utils.hpp"
#include "devtools/asset_paths.hpp"

#include "devtools/animation_runtime_refresh.hpp"

namespace asset_paths = devmode::asset_paths;

namespace {

nlohmann::json animation_manifest_snapshot(const AssetInfo& info) {
    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["animations"] = nlohmann::json::object();
    snapshot["start"] = std::string{};

    nlohmann::json payload = info.manifest_payload();
    if (!payload.is_object()) {
        return snapshot;
    }

    bool has_start = false;
    const auto animations_it = payload.find("animations");
    if (animations_it != payload.end() && animations_it->is_object()) {
        const auto nested_animations_it = animations_it->find("animations");
        if (nested_animations_it != animations_it->end() && nested_animations_it->is_object()) {
            snapshot["animations"] = *nested_animations_it;
        } else {
            snapshot["animations"] = *animations_it;
        }

        const auto nested_start_it = animations_it->find("start");
        if (nested_start_it != animations_it->end()) {
            snapshot["start"] = *nested_start_it;
            has_start = true;
        }
    }

    if (!has_start) {
        const auto root_start_it = payload.find("start");
        if (root_start_it != payload.end()) {
            snapshot["start"] = *root_start_it;
        }
    }

    return snapshot;
}

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

    SDL_FPoint screen_pos = cam.map_to_screen(target->world_xz_point());
    float distance_scale = 1.0f;
    float vertical_scale = 1.0f;
    if (auto* gp = cam.grid_point_for_asset(target)) {
        screen_pos = gp->screen_position();
        distance_scale = std::max(0.0001f, gp->perspective_scale());
        vertical_scale = std::max(0.0001f, gp->vertical_scale());
    }

    float scaled_sw = base_sw * distance_scale;
    float scaled_sh = base_sh * distance_scale;
    float final_visible_h = scaled_sh * vertical_scale;

    int sw = std::max(1, static_cast<int>(std::round(scaled_sw)));
    int sh = std::max(1, static_cast<int>(std::round(final_visible_h)));
    if (sw <= 0 || sh <= 0) return;

        SDL_Point world_point{ target->world_xz_point().x, target->world_xz_point().y };
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

class Section_Tags : public DockableCollapsible {
  public:
    Section_Tags() : DockableCollapsible("Tags", false) { set_visible_height(480); }
    ~Section_Tags() override = default;

    void set_ui(AssetInfoUI* ui) { ui_ = ui; }

    void build() override {
      widgets_.clear();
      DockableCollapsible::Rows rows;
      if (!info_) {
        auto placeholder = std::make_unique<ReadOnlyTextBoxWidget>( "", "No asset selected. Select an asset from the library or scene to view and edit its information.");
        rows.push_back({ placeholder.get() });
        widgets_.push_back(std::move(placeholder));
        set_rows(rows);
        return;
      }

      if (!tag_editor_) {
        tag_editor_ = std::make_unique<TagEditorWidget>(TagEditorWidget::Mode::AssetInfoOverhaul);
        tag_editor_->set_on_changed([this](const std::vector<std::string>& tags,
                                           const std::vector<std::string>& anti_tags) {
          if (!info_) return;
          info_->set_tags(tags);
          info_->set_anti_tags(anti_tags);
          if (ui_) {
            ui_->enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                       "Tags",
                                       [this]() {
                                         tag_utils::notify_tags_changed();
                                         if (ui_) {
                                           ui_->sync_target_tags();
                                         }
                                       });
          }
        });
      }

      tag_editor_->set_subject_asset_name(info_->name);
      tag_editor_->set_tags(info_->tags, info_->anti_tags);
      rows.push_back({ tag_editor_.get() });

      if (!apply_btn_) {
        apply_btn_ = std::make_unique<DMButton>("Apply Settings", &DMStyles::AccentButton(), 180, DMButton::height());
      }
      auto w_apply = std::make_unique<ButtonWidget>(apply_btn_.get(), [this]() {
        if (ui_) ui_->request_apply_section(AssetInfoSectionId::Tags);
      });
      rows.push_back({ w_apply.get() });
      widgets_.push_back(std::move(w_apply));

      set_rows(rows);
    }

    void layout() override { DockableCollapsible::layout(); }

    bool handle_event(const SDL_Event& e) override {
      return DockableCollapsible::handle_event(e);
    }

    void render_content(SDL_Renderer* ) const override {}

  private:
    std::unique_ptr<TagEditorWidget> tag_editor_;
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::unique_ptr<DMButton> apply_btn_;
    AssetInfoUI* ui_ = nullptr;

  protected:
    std::string_view lock_settings_namespace() const override { return "asset_info"; }
    std::string_view lock_settings_id() const override { return "tags"; }
};

class Section_Spacing : public DockableCollapsible {
  public:
    Section_Spacing() : DockableCollapsible("Spacing", false) {}
    ~Section_Spacing() override = default;

    void set_ui(AssetInfoUI* ui) { ui_ = ui; }

    void build() override {
      widgets_.clear();
      DockableCollapsible::Rows rows;
      if (!info_) {
        auto placeholder = std::make_unique<ReadOnlyTextBoxWidget>( "", "No asset selected. Select an asset from the library or scene to view and edit its information.");
        rows.push_back({ placeholder.get() });
        widgets_.push_back(std::move(placeholder));
        set_rows(rows);
        return;
      }
      s_min_same_ = std::make_unique<DMSlider>( "Min Distance From Same Type", 0, 2000, std::max(0, info_->min_same_type_distance));
      s_min_all_  = std::make_unique<DMSlider>( "Min Distance From All Assets", 0, 2000, std::max(0, info_->min_distance_all));
      int neighbor_distance = info_->NeighborSearchRadius > 0 ? info_->NeighborSearchRadius : 500;
      neighbor_distance = std::clamp(neighbor_distance, 20, 1000);
      s_neighbor_search_ = std::make_unique<DMSlider>("Neighbor Search Distance", 20, 1000, neighbor_distance);

      auto w_same = std::make_unique<SliderWidget>(s_min_same_.get());
      rows.push_back({ w_same.get() });
      widgets_.push_back(std::move(w_same));

      auto w_all = std::make_unique<SliderWidget>(s_min_all_.get());
      rows.push_back({ w_all.get() });
      widgets_.push_back(std::move(w_all));

      auto w_neighbor = std::make_unique<SliderWidget>(s_neighbor_search_.get());
      rows.push_back({ w_neighbor.get() });
      widgets_.push_back(std::move(w_neighbor));

      if (!apply_btn_) {
        apply_btn_ = std::make_unique<DMButton>("Apply Settings", &DMStyles::AccentButton(), 180, DMButton::height());
      }
      auto w_apply = std::make_unique<ButtonWidget>(apply_btn_.get(), [this]() {
        if (ui_) ui_->request_apply_section(AssetInfoSectionId::Spacing);
      });
      rows.push_back({ w_apply.get() });
      widgets_.push_back(std::move(w_apply));

      set_rows(rows);
    }

    void layout() override {
      DockableCollapsible::layout();
    }

    bool handle_event(const SDL_Event& e) override {
      bool used = DockableCollapsible::handle_event(e);
      if (!info_ || !expanded_) return used;

      if (!used) {
        if (s_min_same_ && s_min_same_->handle_event(e)) used = true;
        if (s_min_all_ && s_min_all_->handle_event(e)) used = true;
        if (s_neighbor_search_ && s_neighbor_search_->handle_event(e)) used = true;
      }

      bool changed = false;

      if (s_min_same_ && info_->min_same_type_distance != s_min_same_->value()) {
        int v = std::max(0, s_min_same_->value());
        info_->set_min_same_type_distance(v);
        changed = true;
      }
      if (s_min_all_ && info_->min_distance_all != s_min_all_->value()) {
        int v = std::max(0, s_min_all_->value());
        info_->set_min_distance_all(v);
        changed = true;
      }
      if (s_neighbor_search_ && info_->NeighborSearchRadius != s_neighbor_search_->value()) {
        int v = std::clamp(s_neighbor_search_->value(), 20, 1000);
        info_->set_neighbor_search_radius(v);
        changed = true;
      }
      if (changed) {
        auto on_success = [this]() {
          if (ui_) {
            ui_->sync_target_spacing_settings();
          }
        };
        if (ui_) {
          ui_->enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                     "Spacing",
                                     on_success);
        } else {
          on_success();
        }
      }
      return used || changed;
    }

    void render_content(SDL_Renderer* ) const override {}

  private:
    std::unique_ptr<DMSlider> s_min_same_;
    std::unique_ptr<DMSlider> s_min_all_;
    std::unique_ptr<DMSlider> s_neighbor_search_;
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::unique_ptr<DMButton> apply_btn_;
    AssetInfoUI* ui_ = nullptr;

  protected:
    std::string_view lock_settings_namespace() const override { return "asset_info"; }
    std::string_view lock_settings_id() const override { return "spacing"; }
};

using vibble::strings::to_lower_copy;

std::optional<long long> file_mtime_ns(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return static_cast<long long>(ns);
}

void configure_panel_for_container(DockableCollapsible* panel) {
    if (!panel) {
        return;
    }

    panel->set_floatable(false);
    panel->set_close_button_enabled(false);
    panel->set_show_header(true);
    panel->set_scroll_enabled(false);
    panel->reset_scroll();
    panel->set_visible(true);
    panel->force_pointer_ready();
    panel->setLocked(false);
    panel->set_embedded_focus_state(false);
    panel->set_embedded_interaction_enabled(true);
}

bool read_pixel(SDL_Renderer* renderer, const SDL_Rect& rect, Uint32 format, Uint32& out_pixel) {
    out_pixel = 0;
    if (!renderer) {
        return false;
    }

    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &rect);
    if (!captured) {
        return false;
    }

    SDL_Surface* working = captured;
    if (format != 0 && format != static_cast<Uint32>(captured->format)) {
        working = SDL_ConvertSurface(captured, static_cast<SDL_PixelFormat>(format));
        SDL_DestroySurface(captured);
        captured = nullptr;
        if (!working) {
            return false;
        }
    }

    if (!working->pixels) {
        SDL_DestroySurface(working);
        return false;
    }

    out_pixel = *static_cast<const Uint32*>(working->pixels);
    SDL_DestroySurface(working);
    return true;
}

std::string resolve_asset_manifest_key(devmode::core::ManifestStore* store, const std::string& selection) {
    if (!store) return {};

    std::string trimmed = selection;
    if (trimmed.empty()) {
        return {};
    }

    if (auto resolved = store->resolve_asset_name(trimmed)) {
        return *resolved;
    }

    const std::string target = to_lower_copy(trimmed);
    for (const auto& view : store->assets()) {
        if (!view || !view.data || !view.data->is_object()) {
            continue;
        }
        const auto& asset_json = *view.data;
        std::string asset_name = asset_json.value("asset_name", view.name);
        if (!asset_name.empty() && to_lower_copy(asset_name) == target) {
            return view.name;
        }
        auto dir_it = asset_json.find("asset_directory");
        if (dir_it != asset_json.end() && dir_it->is_string()) {
            try {
                std::filesystem::path dir = dir_it->get<std::string>();
                if (!dir.empty()) {
                    std::string folder = to_lower_copy(dir.filename().string());
                    if (!folder.empty() && folder == target) {
                        return view.name;
                    }
                    std::string normalized = to_lower_copy(dir.lexically_normal().generic_string());
                    if (!normalized.empty() && normalized == target) {
                        return view.name;
                    }
                }
            } catch (...) {
            }
        }
    }

    return {};
}

bool copy_section_from_source(AssetInfoSectionId section_id, const nlohmann::json& source, nlohmann::json& target) {
    if (!target.is_object()) return false;
    bool changed = false;
    auto copy_key = [&](const char* key) {
        auto it = source.find(key);
        if (it != source.end()) {
            if (!target.contains(key) || target[key] != *it) {
                target[key] = *it;
                return true;
            }
        } else if (target.contains(key)) {
            target.erase(key);
            return true;
        }
        return false;
};

    switch (section_id) {
        case AssetInfoSectionId::BasicInfo: {
            changed |= copy_key("asset_type");
            if (source.contains("size_settings") && source["size_settings"].is_object()) {
                if (!target.contains("size_settings") || target["size_settings"] != source["size_settings"]) {
                    target["size_settings"] = source["size_settings"];
                    changed = true;
                }
            } else if (target.contains("size_settings")) {
                target.erase("size_settings");
                changed = true;
            }
            changed |= copy_key("starting_health");
            changed |= copy_key("can_invert");

            changed |= copy_key("tileable");
            changed |= copy_key("tillable");
            break;
        }
        case AssetInfoSectionId::Tags:
            changed |= copy_key("tags");
            changed |= copy_key("anti_tags");
            break;
        case AssetInfoSectionId::Spacing:
            changed |= copy_key("min_same_type_distance");
            changed |= copy_key("min_distance_all");
            changed |= copy_key("neighbor_search_distance");
            break;
    }
    return changed;
}

}

AssetInfoUI::AssetInfoUI() {
    rebuild_default_sections();
    if (!configure_btn_) {
        configure_btn_ = std::make_unique<DMButton>("Configure Animations", &DMStyles::CreateButton(), 220, DMButton::height());
    }
    if (!configure_btn_widget_) {
        configure_btn_widget_ = std::make_unique<ButtonWidget>(configure_btn_.get(), [this]() {
            if (!animation_editor_window_) {
                return;
            }
            if (animation_editor_window_->is_visible()) {
                animation_editor_window_->set_visible(false);
            } else if (info_) {
                if (animation_editor_rect_.w > 0 && animation_editor_rect_.h > 0) {
                    animation_editor_window_->set_bounds(animation_editor_rect_);
                }
                animation_editor_window_->set_visible(true);
            }
        });
    }
    if (!animation_editor_window_) {
        animation_editor_window_ = std::make_unique<animation_editor::AnimationEditorWindow>();
        if (animation_editor_window_) {
            animation_editor_window_->set_manifest_store(manifest_store_);
            animation_editor_window_->set_on_document_saved([this]() { this->on_animation_document_saved(); });
        }
    }

    if (!duplicate_btn_) {
        duplicate_btn_ = std::make_unique<DMButton>("Duplicate Asset", &DMStyles::FooterToggleButton(), 220, DMButton::height());
    }
    if (!duplicate_btn_widget_) {
        duplicate_btn_widget_ = std::make_unique<ButtonWidget>(duplicate_btn_.get(), [this]() {
            if (!info_) return;
            showing_duplicate_popup_ = true;
            duplicate_asset_name_.clear();
        });
    }

    if (!delete_btn_) {
        delete_btn_ = std::make_unique<DMButton>("Delete Asset", &DMStyles::DeleteButton(), 220, DMButton::height());
    }
    if (!delete_btn_widget_) {
        delete_btn_widget_ = std::make_unique<ButtonWidget>(delete_btn_.get(), [this]() {
            this->request_delete_current_asset();
        });
    }

    container_.set_header_text_provider([this]() { return info_ ? info_->name : std::string(); });

    container_.set_scrollbar_visible(true);
    container_.set_content_clip_enabled(false);

    container_.set_layout_function([this](const SlidingWindowContainer::LayoutContext& ctx) {
        int y = ctx.content_top;
        section_bounds_.resize(sections_.size());
        const int embed_screen_h = last_screen_h_ > 0 ? last_screen_h_ : std::max(1, ctx.content_width);
        for (size_t i = 0; i < sections_.size(); ++i) {
            auto& section = sections_[i];
            if (!section) {
                section_bounds_[i] = SDL_Rect{0,0,0,0};
                continue;
            }
            int measured = section->embedded_height(ctx.content_width, embed_screen_h);
            SDL_Rect rect{ctx.content_x, y - ctx.scroll_value, ctx.content_width, measured};
            section_bounds_[i] = rect;
            section->set_rect(rect); // keep event hit-testing aligned with current scroll/layout
            y += measured + ctx.gap;
        }
        if (configure_btn_widget_) {
            configure_btn_widget_->set_rect(SDL_Rect{ctx.content_x, y - ctx.scroll_value, ctx.content_width, DMButton::height()});
            y += DMButton::height() + ctx.gap;
        }
        if (duplicate_btn_widget_) {
            duplicate_btn_widget_->set_rect(SDL_Rect{ctx.content_x, y - ctx.scroll_value, ctx.content_width, DMButton::height()});
            y += DMButton::height() + ctx.gap;
        }
        if (delete_btn_widget_) {
            delete_btn_widget_->set_rect(SDL_Rect{ctx.content_x, y - ctx.scroll_value, ctx.content_width, DMButton::height()});
            y += DMButton::height() + ctx.gap;
        }
        return y;
    });

    container_.set_render_function([this](SDL_Renderer* renderer) {
        for (size_t i = 0; i < sections_.size(); ++i) {
            auto& section = sections_[i];
            if (!section) continue;
            SDL_Rect bounds = (i < section_bounds_.size()) ? section_bounds_[i] : SDL_Rect{0,0,0,0};
            section->render_embedded(renderer, bounds, last_screen_w_, last_screen_h_);
        }
        if (configure_btn_) configure_btn_->render(renderer);
        if (duplicate_btn_) duplicate_btn_->render(renderer);
        if (delete_btn_) delete_btn_->render(renderer);
    });

    container_.set_on_close([this]() { this->close(); });

    container_.set_update_function([this](const Input& input, int screen_w, int screen_h) {
        if (panel_bounds_override_active_) {
            container_.set_panel_bounds_override(panel_bounds_override_);
        } else {
            SDL_Rect usable = DockManager::instance().usableRect();
            if (usable.w > 0 && usable.h > 0) {
                int panel_x = screen_w - std::max(screen_w / 3, 320);
                panel_x = std::clamp(panel_x, 0, screen_w);
                int panel_w = std::max(0, screen_w - panel_x);
                SDL_Rect bounds{panel_x, usable.y, panel_w, usable.h};
                container_.set_panel_bounds_override(bounds);
            } else {
                container_.clear_panel_bounds_override();
            }
        }
        std::vector<bool> previously_expanded;
        std::vector<int> previous_heights;
        previously_expanded.reserve(sections_.size());
        previous_heights.reserve(sections_.size());
        for (const auto& section : sections_) {
            previously_expanded.push_back(section->is_expanded());
            previous_heights.push_back(section->height());
        }

        for (auto& section : sections_) {
            section->update(input, screen_w, screen_h);
        }

        bool expansion_changed = false;
        bool height_changed = false;
        for (size_t i = 0; i < sections_.size(); ++i) {
            if (sections_[i]->is_expanded() != previously_expanded[i]) {
                expansion_changed = true;
                break;
            }
        }

        if (!height_changed) {
            for (size_t i = 0; i < sections_.size(); ++i) {
                if (sections_[i]->height() != previous_heights[i]) {
                    height_changed = true;
                    break;
                }
            }
        }

        if (expansion_changed || height_changed) {
            container_.request_layout();
        }
    });

    container_.set_event_function([this](const SDL_Event& e) {
        (void)handle_section_focus_event(e);
        auto handle_section_event = [this, &e](DockableCollapsible* section) -> bool {
            if (!section) {
                return false;
            }
            if (section->handle_event(e)) {
                container_.request_layout();
                if (focused_section_ != section) {
                    focus_section(section);
                }
                return true;
            }
            return false;
        };
        if (focused_section_ && handle_section_event(focused_section_)) {
            return true;
        }
        for (auto& section : sections_) {
            DockableCollapsible* candidate = section.get();
            if (!candidate || candidate == focused_section_) {
                continue;
            }
            if (handle_section_event(candidate)) {
                return true;
            }
        }
        if (configure_btn_widget_ && configure_btn_widget_->handle_event(e)) {
            return true;
        }
        if (duplicate_btn_widget_ && duplicate_btn_widget_->handle_event(e)) return true;
        if (delete_btn_widget_ && delete_btn_widget_->handle_event(e)) return true;
        return false;
    });
}

AssetInfoUI::~AssetInfoUI() {
    apply_camera_override(false);
    if (assets_) {

    }
    if (assets_ && forcing_high_quality_rendering_) {

    }
    forcing_high_quality_rendering_ = false;
    cancel_color_sampling(true);
    if (color_sampling_cursor_handle_) {
        SDL_DestroyCursor(color_sampling_cursor_handle_);
        color_sampling_cursor_handle_ = nullptr;
    }
}

void AssetInfoUI::set_assets(Assets* a) {
    if (assets_ == a) return;
    if (assets_ && forcing_high_quality_rendering_) {

        forcing_high_quality_rendering_ = false;
    }

    if (assets_) {

    }
    if (camera_override_active_) {
        apply_camera_override(false);
    }
    assets_ = a;
    set_manifest_store(assets_ ? assets_->manifest_store() : nullptr);
    if (asset_selector_) {
        asset_selector_->set_assets(assets_);
    }
    if (animation_editor_window_) {
        animation_editor_window_->set_assets(assets_);
    }
    if (visible_) {
        apply_camera_override(true);
    }
    validate_target_asset();
}

void AssetInfoUI::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
    if (animation_editor_window_) {
        animation_editor_window_->set_manifest_store(manifest_store_);
    }
}

void AssetInfoUI::set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) {
    save_coordinator_ = coordinator;
    if (animation_editor_window_) {
        animation_editor_window_->set_save_coordinator(coordinator);
    }
}

void AssetInfoUI::set_target_asset(Asset* a) {
    target_asset_ = a;
    validate_target_asset();
    if (animation_editor_window_) {
        animation_editor_window_->set_target_asset(target_asset_);
    }
}

void AssetInfoUI::set_panel_bounds_override(const SDL_Rect& bounds) {
    panel_bounds_override_ = bounds;
    panel_bounds_override_active_ = bounds.w > 0 && bounds.h > 0;
    container_.request_layout();
}

void AssetInfoUI::clear_panel_bounds_override() {
    panel_bounds_override_active_ = false;
    panel_bounds_override_ = SDL_Rect{0, 0, 0, 0};
    container_.request_layout();
}

void AssetInfoUI::set_info(const std::shared_ptr<AssetInfo>& info) {
    info_ = info;
    container_.reset_scroll();
    if (asset_selector_) asset_selector_->close();
    if (animation_editor_window_) {
        try {
            animation_editor_window_->set_manifest_store(manifest_store_);
            animation_editor_window_->set_on_animation_properties_changed([this](const std::string& animation_id, const nlohmann::json& properties) {
                if (!info_) {
                    return;
                }
                if (!info_->update_animation_properties(animation_id, properties)) {
                    return;
                }

                auto refresh = [this]() { this->refresh_loaded_asset_instances(); };
                if (!enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                           "Animation properties",
                                           refresh)) {
                    // Even if persistence fails, keep the runtime view in sync.
                    refresh();
                }
            });
            animation_editor_window_->set_info(info_);
        } catch (const std::exception& ex) {
            SDL_Log("AssetInfoUI: failed to configure animation editor for %s: %s", info_ ? info_->name.c_str() : "<null>", ex.what());
            animation_editor_window_->clear_info();
            animation_editor_window_->set_visible(false);
        } catch (...) {
            SDL_Log("AssetInfoUI: failed to configure animation editor for %s due to unknown error.", info_ ? info_->name.c_str() : "<null>");
            animation_editor_window_->clear_info();
            animation_editor_window_->set_visible(false);
        }
    }
    for (auto& s : sections_) {
        try {
            s->set_info(info_);
            s->reset_scroll();
            s->build();
        } catch (const std::exception& ex) {
            SDL_Log("AssetInfoUI: failed to build section while loading %s: %s", info_ ? info_->name.c_str() : "<null>", ex.what());
        } catch (...) {
            SDL_Log("AssetInfoUI: failed to build section while loading %s due to unknown error.", info_ ? info_->name.c_str() : "<null>");
        }
    }
    container_.request_layout();
}

void AssetInfoUI::clear_info() {
    if (assets_) {

    }
    if (assets_ && forcing_high_quality_rendering_) {

        forcing_high_quality_rendering_ = false;
    }
    info_.reset();
    container_.reset_scroll();
    if (asset_selector_) asset_selector_->close();
    pending_animation_editor_open_ = false;
    if (animation_editor_window_) {
        try {
            animation_editor_window_->clear_info();
            animation_editor_window_->set_visible(false);
        } catch (const std::exception& ex) {
            SDL_Log("AssetInfoUI: failed to reset animation editor: %s", ex.what());
        } catch (...) {
            SDL_Log("AssetInfoUI: failed to reset animation editor due to unknown error.");
        }
    }
    for (auto& s : sections_) {
        try {
            s->set_info(nullptr);
            s->reset_scroll();
            s->build();
        } catch (const std::exception& ex) {
            SDL_Log("AssetInfoUI: failed to reset section: %s", ex.what());
        } catch (...) {
            SDL_Log("AssetInfoUI: failed to reset section due to unknown error.");
        }
    }
    target_asset_ = nullptr;
    clear_section_focus();
}

void AssetInfoUI::open()  {
    visible_ = true;
    container_.open();
    apply_camera_override(true);
}
void AssetInfoUI::close() {
    if (!visible_) return;
    pending_animation_editor_open_ = false;
    apply_camera_override(false);
    visible_ = false;
    container_.close();
    clear_section_focus();
    if (assets_) {

    }
    if (animation_editor_window_) animation_editor_window_->set_visible(false);
    if (asset_selector_) asset_selector_->close();
    if (assets_ && forcing_high_quality_rendering_) {

        forcing_high_quality_rendering_ = false;
    }
    if (save_coordinator_) {
        save_coordinator_->flush_now("AssetInfoUI close");
    }
}
void AssetInfoUI::toggle(){
    if (visible_) {
        close();
    } else {
        open();
    }
}

void AssetInfoUI::open_animation_editor_panel() {
    if (!animation_editor_window_ || !info_) {
        pending_animation_editor_open_ = false;
        return;
    }

    pending_animation_editor_open_ = true;

    if (last_screen_w_ > 0 && last_screen_h_ > 0) {
        layout_widgets(last_screen_w_, last_screen_h_);
        if (animation_editor_rect_.w > 0 && animation_editor_rect_.h > 0) {
            animation_editor_window_->set_bounds(animation_editor_rect_);
            animation_editor_window_->set_visible(true);
            pending_animation_editor_open_ = false;
        }
    }
}

bool AssetInfoUI::is_locked() const {
    for (const auto& section : sections_) {
        if (section && section->isLocked()) {
            return true;
        }
    }
    return false;
}

void AssetInfoUI::layout_widgets(int screen_w, int screen_h) const {
    container_.prepare_layout(screen_w, screen_h);
    const SDL_Rect& panel = container_.panel_rect();
    int editor_width = panel.x;
    int editor_y = panel.y;
    int editor_height = panel.h > 0 ? panel.h : std::max(0, screen_h - editor_y);
    if (editor_width <= 0) {

        editor_width = std::max( screen_w - std::max(panel.w, std::max(screen_w / 3, 320)), screen_w / 3);
    }
    if (editor_height <= 0) {
        editor_height = std::max(0, screen_h - editor_y);
    }
    if (editor_width <= 0 || editor_height <= 0) {
        animation_editor_rect_ = SDL_Rect{0, 0, 0, 0};
    } else {
        animation_editor_rect_ = SDL_Rect{0, editor_y, editor_width, editor_height};
    }

}

bool AssetInfoUI::handle_event(const SDL_Event& e) {

    if (color_sampling_active_) {
        const bool pointer_event =
            (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION);
        if (pointer_event) {
            color_sampling_cursor_.x = (e.type == SDL_EVENT_MOUSE_MOTION)
                                           ? static_cast<int>(std::lround(e.motion.x))
                                           : static_cast<int>(std::lround(e.button.x));
            color_sampling_cursor_.y = (e.type == SDL_EVENT_MOUSE_MOTION)
                                           ? static_cast<int>(std::lround(e.motion.y))
                                           : static_cast<int>(std::lround(e.button.y));
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            if (last_renderer_) {
                SDL_Rect sample_rect{ color_sampling_cursor_.x, color_sampling_cursor_.y, 1, 1 };
                Uint32 pixel = 0;
                if (read_pixel(last_renderer_, sample_rect, SDL_PIXELFORMAT_ARGB8888, pixel)) {
                    if (const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_ARGB8888)) {
                        Uint8 r = 0, g = 0, b = 0, a = 0;
                        SDL_GetRGBA(pixel, fmt, nullptr, &r, &g, &b, &a);
                        complete_color_sampling(SDL_Color{r, g, b, a});
                        return true;
                    }
                }
            }

            cancel_color_sampling(true);
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            cancel_color_sampling(false);
            return true;
        }

        switch (e.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_TEXT_INPUT:
                return true;
            default:
                break;
        }
    }

    if (auto* active_dd = DMDropdown::active_dropdown()) {
        if (active_dd->handle_event(e)) {
            return true;
        }
    }


    const bool pointer_event =
        (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION);
    const bool wheel_event = (e.type == SDL_EVENT_MOUSE_WHEEL);
    SDL_Point pointer{0, 0};
    if (pointer_event) {
        pointer.x = (e.type == SDL_EVENT_MOUSE_MOTION) ? e.motion.x : e.button.x;
        pointer.y = (e.type == SDL_EVENT_MOUSE_MOTION) ? e.motion.y : e.button.y;
    }

    if (asset_selector_ && asset_selector_->visible()) {
        if (asset_selector_->handle_event(e)) return true;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            asset_selector_->close();
            return true;
        }
        if (pointer_event) {
            if (asset_selector_->is_point_inside(pointer.x, pointer.y)) {
                return true;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                asset_selector_->close();
                return true;
            }
        } else if (wheel_event) {
            int mx = 0;
            int my = 0;
            sdl_mouse_util::GetMouseState(&mx, &my);
            if (asset_selector_->is_point_inside(mx, my)) {
                return true;
            }
        }
    }

    if (!visible_) return false;

    if (showing_delete_popup_) {
        if (handle_delete_modal_event(e)) {
            return true;
        }
        switch (e.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_TEXT_INPUT:
                return true;
            default:
                break;
        }
    }

    if (showing_duplicate_popup_) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_RETURN) {
                if (duplicate_current_asset(duplicate_asset_name_)) {
                    duplicate_asset_name_.clear();
                }
                showing_duplicate_popup_ = false;
                return true;
            } else if (e.key.key == SDLK_ESCAPE) {
                showing_duplicate_popup_ = false;
                duplicate_asset_name_.clear();
                return true;
            } else if (e.key.key == SDLK_BACKSPACE) {
                if (!duplicate_asset_name_.empty()) duplicate_asset_name_.pop_back();
                return true;
            }
        } else if (e.type == SDL_EVENT_TEXT_INPUT) {
            duplicate_asset_name_ += e.text.text;
            return true;
        }
    }

    if (animation_editor_window_ && animation_editor_window_->is_visible()) {
        if (animation_editor_window_->handle_event(e)) {
            return true;
        }
    }

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        close();
        return true;
    }

    if (container_.handle_event(e)) {
        return true;
    }

    return false;
}

void AssetInfoUI::update(const Input& input, int screen_w, int screen_h) {
    validate_target_asset();
    last_screen_w_ = screen_w;
    last_screen_h_ = screen_h;
    layout_widgets(screen_w, screen_h);

    if (animation_editor_window_) {
        animation_editor_window_->set_bounds(animation_editor_rect_);
        if (pending_animation_editor_open_ && info_ &&
            animation_editor_rect_.w > 0 && animation_editor_rect_.h > 0) {
            animation_editor_window_->set_visible(true);
            pending_animation_editor_open_ = false;
        }
        if (animation_editor_window_->is_visible()) {
            animation_editor_window_->update(input, screen_w, screen_h);
        }
    }


    const bool need_high_quality = false;
    if (assets_) {
        if (need_high_quality != forcing_high_quality_rendering_) {

            forcing_high_quality_rendering_ = need_high_quality;
        }
    } else {
        forcing_high_quality_rendering_ = false;
    }

    if (!visible_) return;


    if (info_ && asset_selector_ && asset_selector_->visible()) {
        asset_selector_->update(input);
        const SDL_Rect& panel = container_.panel_rect();
        DockManager::SlidingParentInfo parent;
        parent.bounds = panel;
        parent.padding = DMSpacing::panel_padding();
        parent.anchor_left = true;
        parent.align_top = true;
        asset_selector_->layout_with_parent(parent);
    }

    container_.update(input, screen_w, screen_h);

    layout_widgets(screen_w, screen_h);

    if (showing_delete_popup_) {
        update_delete_modal_geometry(screen_w, screen_h);
    }
    if (showing_duplicate_popup_) {
        SDL_StartTextInput(SDL_GetKeyboardFocus());
    }

    // Ensure debounced saves still flush even if the global coordinator tick is skipped.
    if (save_coordinator_) {
        save_coordinator_->tick();
    }
}

void AssetInfoUI::render(SDL_Renderer* r, int screen_w, int screen_h) const {
    if (!visible_) return;

    layout_widgets(screen_w, screen_h);
    last_renderer_ = r;

    container_.render(r, screen_w, screen_h);
    if (animation_editor_window_ && animation_editor_window_->is_visible()) {
        animation_editor_window_->render(r);
    }

    if (asset_selector_ && asset_selector_->visible())
        asset_selector_->render(r);

    DMDropdown::render_active_options(r);

    if (color_sampling_active_ && r) {
        SDL_Rect sample_rect{ color_sampling_cursor_.x, color_sampling_cursor_.y, 1, 1 };
        Uint32 pixel = 0;
        if (read_pixel(r, sample_rect, SDL_PIXELFORMAT_ARGB8888, pixel)) {
            if (const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_ARGB8888)) {
                Uint8 r = 0, g = 0, b = 0, a = 0;
                SDL_GetRGBA(pixel, fmt, nullptr, &r, &g, &b, &a);
                const_cast<AssetInfoUI*>(this)->color_sampling_preview_ = SDL_Color{r, g, b, a};
                const_cast<AssetInfoUI*>(this)->color_sampling_preview_valid_ = true;
            } else {
                const_cast<AssetInfoUI*>(this)->color_sampling_preview_valid_ = false;
            }
        } else {
            const_cast<AssetInfoUI*>(this)->color_sampling_preview_valid_ = false;
        }

        const int preview_size = 48;
        SDL_Rect preview_rect{ color_sampling_cursor_.x + 18,
                               color_sampling_cursor_.y + 18,
                               preview_size,
                               preview_size };
        SDL_Rect inner_rect{ preview_rect.x + 4,
                             preview_rect.y + 4,
                             std::max(0, preview_rect.w - 8), std::max(0, preview_rect.h - 8) };
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_Color border = DMStyles::Border();
        SDL_Color bg = dm_draw::DarkenColor(DMStyles::PanelBG(), 0.1f);
        SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 220);
        sdl_render::FillRect(r, &preview_rect);
        SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
        sdl_render::Rect(r, &preview_rect);
        if (color_sampling_preview_valid_) {
            SDL_Color fill = color_sampling_preview_;
            SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
            sdl_render::FillRect(r, &inner_rect);
            SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
            sdl_render::Rect(r, &inner_rect);
        }
    }

    if (showing_duplicate_popup_) {
        SDL_Rect box{ screen_w/2 - 150, screen_h/2 - 40, 300, 80 };
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const SDL_Color panel_bg = DMStyles::PanelBG();
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();
        const int corner_radius = DMStyles::CornerRadius();
        const int bevel_depth = DMStyles::BevelDepth();
        dm_draw::DrawBeveledRect( r, box, corner_radius, bevel_depth, panel_bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        const SDL_Color panel_border = DMStyles::Border();
        dm_draw::DrawRoundedOutline( r, box, corner_radius, 1, panel_border);

        SDL_Rect input_rect{ box.x + 8, box.y + 8, box.w - 16, box.h - 16 };
        const DMTextBoxStyle& textbox = DMStyles::TextBox();
        dm_draw::DrawBeveledRect( r, input_rect, corner_radius, bevel_depth, textbox.bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        dm_draw::DrawRoundedOutline( r, input_rect, corner_radius, 1, textbox.border);

        const int text_padding = 12 + bevel_depth;
        const int interior_h = std::max(0, input_rect.h - 2 * bevel_depth);
        TTF_Font* font = devmode::utils::load_font(18);
        if (font) {
            std::string display = duplicate_asset_name_.empty() ? "Enter asset name..." : duplicate_asset_name_;
            SDL_Color color = duplicate_asset_name_.empty() ? textbox.label.color : textbox.text;
            int available_w = input_rect.w - 2 * text_padding;
            if (available_w < 0) available_w = 0;
            int tw = 0;
            int th = 0;
            std::string render_text = display;
            if (ttf_util::GetStringSize(font, render_text, &tw, &th) && tw > available_w) {
                const std::string ellipsis = "...";
                std::string base = display;
                while (!base.empty()) {
                    base.pop_back();
                    std::string candidate = base + ellipsis;
                    if (ttf_util::GetStringSize(font, candidate, &tw, &th) && tw <= available_w) {
                        render_text = std::move(candidate);
                        break;
                    }
                }
                if (base.empty()) {
                    render_text = ellipsis;
                    (void)ttf_util::GetStringSize(font, render_text, &tw, &th);
                }
            } else {
                (void)ttf_util::GetStringSize(font, render_text, &tw, &th);
            }

            SDL_Surface* surf = ttf_util::RenderTextBlended(font, render_text.c_str(), color);
            if (surf) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
                SDL_DestroySurface(surf);
                if (tex) {
                    const int text_area_h = std::max(0, interior_h - th);
                    int text_y = input_rect.y + bevel_depth + text_area_h / 2;
                    text_y = std::max(text_y, input_rect.y + bevel_depth);
                    text_y = std::min(text_y, input_rect.y + input_rect.h - bevel_depth - th);
                    SDL_Rect dst{ input_rect.x + text_padding,
                                  text_y,
                                  tw,
                                  th };
                    sdl_render::Texture(r, tex, nullptr, &dst);
                    SDL_DestroyTexture(tex);
                }
            }
        }
    }

    if (showing_delete_popup_) {
        const SDL_Color panel_bg = DMStyles::PanelBG();
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();
        const int corner_radius = DMStyles::CornerRadius();
        const int bevel_depth = DMStyles::BevelDepth();
        dm_draw::DrawBeveledRect( r, delete_modal_rect_, corner_radius, bevel_depth, panel_bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        const SDL_Color panel_border = DMStyles::Border();
        dm_draw::DrawRoundedOutline( r, delete_modal_rect_, corner_radius, 1, panel_border);

        auto render_button = [&](const SDL_Rect& rect, bool hovered, bool pressed, const std::string& caption, const DMButtonStyle& style) {
            SDL_Color bg = style.bg;
            if (pressed) bg = style.press_bg; else if (hovered) bg = style.hover_bg;
            dm_draw::DrawBeveledRect( r, rect, corner_radius, bevel_depth, bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
            dm_draw::DrawRoundedOutline( r, rect, corner_radius, 1, style.border);

            TTF_Font* btn_font = devmode::utils::load_font(style.label.font_size > 0 ? style.label.font_size : 16);
            if (!btn_font) btn_font = devmode::utils::load_font(16);
            if (btn_font) {
                SDL_Surface* text = ttf_util::RenderTextBlended(btn_font, caption.c_str(), style.text);
                if (text) {
                    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, text);
                    SDL_DestroySurface(text);
                    if (tex) {
                        float twf = 0.0f;
                        float thf = 0.0f;
                        SDL_GetTextureSize(tex, &twf, &thf);
                        const int tw = static_cast<int>(std::lround(twf));
                        const int th = static_cast<int>(std::lround(thf));
                        const int interior_h = std::max(0, rect.h - 2 * bevel_depth);
                        int text_y = rect.y + bevel_depth + std::max(0, interior_h - th) / 2;
                        text_y = std::max(text_y, rect.y + bevel_depth);
                        text_y = std::min(text_y, rect.y + rect.h - bevel_depth - th);
                        SDL_Rect dst{ rect.x + (rect.w - tw) / 2, text_y, tw, th };
                        sdl_render::Texture(r, tex, nullptr, &dst);
                        SDL_DestroyTexture(tex);
                    }
                }
            }
};

        render_button(delete_yes_rect_, delete_yes_hovered_, delete_yes_pressed_, "Yes, delete", DMStyles::DeleteButton());
        render_button(delete_no_rect_, delete_no_hovered_, delete_no_pressed_, "Cancel", DMStyles::HeaderButton());
    }

    last_renderer_ = r;
}

void AssetInfoUI::pulse_header() {
    container_.pulse_header();
}

void AssetInfoUI::apply_camera_override(bool enable) {
    if (!assets_) return;
    WarpedScreenGrid& cam = assets_->getView();
    if (enable) {
        if (camera_override_active_) return;
        camera_override_active_ = true;
    } else {
        if (!camera_override_active_) return;
        camera_override_active_ = false;
    }
}

float AssetInfoUI::compute_player_screen_height(const WarpedScreenGrid& cam) const {
    if (!assets_ || !assets_->player) return 1.0f;
    Asset* player_asset = assets_->player;
    if (!player_asset) return 1.0f;

    SDL_Texture* player_frame = player_asset->get_current_frame();
    if (!player_frame && player_asset->info && player_asset->info->animations.count(player_asset->current_animation)) {
        AnimationFrame* frame = player_asset->info->animations[player_asset->current_animation].get_first_frame();
        if (frame && !frame->variants.empty()) {
            player_frame = frame->get_base_texture(0);
        }
    }
    int pw = player_asset->cached_w;
    int ph = player_asset->cached_h;
    if ((pw == 0 || ph == 0) && player_frame) {
        float pwf = 0.0f;
        float phf = 0.0f;
        if (SDL_GetTextureSize(player_frame, &pwf, &phf)) {
            pw = static_cast<int>(std::lround(pwf));
            ph = static_cast<int>(std::lround(phf));
        }
    }
    if ((pw == 0 || ph == 0) && player_asset->info) {
        pw = player_asset->info->original_canvas_width;
        ph = player_asset->info->original_canvas_height;
    }
    if (pw != 0) player_asset->cached_w = pw;
    if (ph != 0) player_asset->cached_h = ph;

    float package_scale = player_asset->current_nearest_variant_scale * player_asset->current_remaining_scale_adjustment;
    if (!std::isfinite(package_scale) || package_scale <= 0.0f) {
        package_scale = 1.0f;
    }

    const world::GridPoint* gp = cam.grid_point_for_asset(player_asset);
    float distance_scale = 1.0f;
    float vertical_scale = 1.0f;
    if (gp) {
        distance_scale = std::max(0.0001f, gp->perspective_scale());
        vertical_scale = std::max(0.0001f, gp->vertical_scale());
    }

    if (ph > 0) {
        float screen_h = static_cast<float>(ph) * package_scale * distance_scale * vertical_scale;
        return screen_h > 0.0f ? screen_h : 1.0f;
    }
    return 1.0f;
}

void AssetInfoUI::render_world_overlay(SDL_Renderer* r, const WarpedScreenGrid& cam) const {
    if (!visible_ || !info_) return;

    validate_target_asset();

    float reference_screen_height = compute_player_screen_height(cam);

    if (auto* basic = static_cast<Section_BasicInfo*>(basic_info_section_)) {
        if (basic->is_expanded()) {
            basic->render_world_overlay(r, cam, target_asset_, reference_screen_height);
        }
    }

}

void AssetInfoUI::refresh_target_asset_scale() {
    if (!info_) return;

    Asset* current_target = target_asset_;
    const bool target_valid = validate_target_asset();
    Asset* validated_target = target_asset_;

    const auto refresh_asset = [&](Asset* asset, bool force_update = false) {
        if (!asset || !asset->info) {
            return false;
        }
        if (!force_update && !asset_matches_current_info(asset)) {
            return false;
        }
        asset->info->set_scale_factor(info_->scale_factor);
        asset->on_scale_factor_changed();
        return true;
};

    bool refreshed_any = false;
    if (assets_) {
        for (Asset* asset : assets_->all) {
            if (refresh_asset(asset)) {
                refreshed_any = true;
            }
        }
    }

    if (target_valid && validated_target) {
        if (refresh_asset(validated_target, true)) {
            refreshed_any = true;
        }
    }

    if (current_target && current_target != validated_target) {
        if (refresh_asset(current_target, true)) {
            refreshed_any = true;
        }
    }

    if (assets_ && refreshed_any) {

        assets_->mark_active_assets_dirty();
    }
}

void AssetInfoUI::begin_color_sampling(const utils::color::RangedColor&,
                                       std::function<void(SDL_Color)> on_sample,
                                       std::function<void()> on_cancel) {
    if (!on_sample) {
        if (on_cancel) {
            on_cancel();
        }
        return;
    }
    cancel_color_sampling(true);
    color_sampling_active_ = true;
    color_sampling_preview_valid_ = false;
    color_sampling_apply_ = std::move(on_sample);
    color_sampling_cancel_ = std::move(on_cancel);
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    color_sampling_cursor_.x = mx;
    color_sampling_cursor_.y = my;
    if (!color_sampling_cursor_handle_) {
        color_sampling_cursor_handle_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    }
    color_sampling_prev_cursor_ = SDL_GetCursor();
    if (color_sampling_cursor_handle_) {
        SDL_SetCursor(color_sampling_cursor_handle_);
    }
}

void AssetInfoUI::cancel_color_sampling(bool silent) {
    if (!color_sampling_active_) {
        return;
    }
    color_sampling_active_ = false;
    color_sampling_preview_valid_ = false;
    if (color_sampling_prev_cursor_) {
        SDL_SetCursor(color_sampling_prev_cursor_);
        color_sampling_prev_cursor_ = nullptr;
    }
    auto cancel_cb = std::move(color_sampling_cancel_);
    color_sampling_apply_ = nullptr;
    color_sampling_cancel_ = nullptr;
    if (!silent && cancel_cb) {
        cancel_cb();
    }
}

bool AssetInfoUI::enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority priority,
                                        const std::string& label,
                                        std::function<void()> on_success) {
    if (!info_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] No asset selected; save skipped.");
        return false;
    }

    info_->mark_dirty();

    auto run_after_save = [this, cb = std::move(on_success)]() mutable {
        if (cb) {
            cb();
            cb = nullptr;
        }
    };

    if (save_coordinator_ && manifest_store_) {
        nlohmann::json payload = info_->manifest_payload();
        const std::string intent_label = label.empty() ? std::string("Asset ") + info_->name : label;
        save_coordinator_->enqueue_manifest_asset(
            info_->name,
            std::move(payload),
            priority,
            intent_label,
            [run_after_save]() mutable { run_after_save(); });
        if (priority == devmode::core::DevSaveCoordinator::Priority::Immediate) {
            save_coordinator_->flush_now(intent_label);
        }
        return true;
    }

    if (!manifest_store_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Manifest store unavailable; falling back to direct commit for '%s'", info_->name.c_str());
        bool committed = false;
        try {
            committed = info_->commit_manifest();
        } catch (const std::exception& ex) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AssetInfoUI] Failed to commit manifest for '%s': %s",
                        info_->name.c_str(),
                        ex.what());
            return false;
        } catch (...) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AssetInfoUI] Unknown failure committing manifest for '%s'",
                        info_->name.c_str());
            return false;
        }
        if (committed) {
            run_after_save();
        }
        return committed;
    }

    nlohmann::json payload = info_->manifest_payload();
    auto session = manifest_store_->begin_asset_edit(info_->name, true);
    if (!session) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to open manifest session for '%s'", info_->name.c_str());
        return false;
    }
    session.data() = payload;
    if (!session.commit()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to commit manifest for '%s'", info_->name.c_str());
        return false;
    }
    run_after_save();
    return true;
}

void AssetInfoUI::complete_color_sampling(SDL_Color color) {
    auto apply_cb = std::move(color_sampling_apply_);
    cancel_color_sampling(true);
    if (apply_cb) {
        apply_cb(color);
    }
}

void AssetInfoUI::apply_section_focus_states() {
    for (auto& section : sections_) {
        if (!section) {
            continue;
        }
        const bool focused = (section.get() == focused_section_);
        section->set_embedded_focus_state(focused);
        section->set_embedded_interaction_enabled(true);
    }
}

void AssetInfoUI::focus_section(DockableCollapsible* section) {
    DockableCollapsible* resolved = nullptr;
    if (section) {
        for (auto& entry : sections_) {
            if (entry.get() == section) {
                resolved = section;
                break;
            }
        }
    }
    DockableCollapsible* previous = focused_section_;
    focused_section_ = resolved;
    collapse_all_except(focused_section_);
    apply_section_focus_states();
    if (focused_section_) {
        focused_section_->force_pointer_ready();
        if (!focused_section_->is_expanded()) {
            focused_section_->set_expanded(true);
        }
    }
    if (previous != focused_section_) {
        container_.request_layout();
    }
}

void AssetInfoUI::clear_section_focus() {
    focus_section(nullptr);
}

void AssetInfoUI::collapse_all_except(DockableCollapsible* keep) {
    for (auto& entry : sections_) {
        DockableCollapsible* section = entry.get();
        if (!section || section == keep) {
            continue;
        }
        if (section->is_expanded()) {
            section->set_expanded(false);
        }
    }
}

DockableCollapsible* AssetInfoUI::section_at_point(SDL_Point p) const {
    for (size_t i = 0; i < sections_.size(); ++i) {
        auto* section = sections_[i].get();
        if (!section) {
            continue;
        }
        SDL_Rect bounds = (i < section_bounds_.size()) ? section_bounds_[i] : section->rect();
        if (bounds.w <= 0 || bounds.h <= 0) {
            continue;
        }
        if (SDL_PointInRect(&p, &bounds)) {
            return section;
        }
    }
    return nullptr;
}

bool AssetInfoUI::handle_section_focus_event(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    SDL_Point pointer = sdl_mouse_util::ButtonPoint(e.button);
    DockableCollapsible* target = section_at_point(pointer);
    if (!target) {
        return false;
    }
    if (target == focused_section_) {
        return false;
    }
    focus_section(target);
    // Keep processing this same event through the newly focused section so
    // controls react on first click instead of requiring a second click.
    return false;
}

void AssetInfoUI::sync_target_tiling_state() {
    if (!info_) return;
    Asset* current_target = target_asset_;
    const bool target_valid = validate_target_asset();
    if (!assets_) {
        return;
    }

    auto compute_tiling = [&](Asset* asset) -> std::optional<Asset::TilingInfo> {
        if (!assets_) return std::nullopt;
        if (!asset || !asset->info) return std::nullopt;
        if (!asset->info->tillable) return std::nullopt;
        return assets_->compute_tiling_for_asset(asset);
};

    auto apply_for_asset = [&](Asset* asset) {
        if (!asset_matches_current_info(asset)) return false;
        if (asset->info) {
            asset->info->set_tillable(info_->tillable);
        }
        if (info_->tillable) {
            auto t = compute_tiling(asset);
            if (t && t->is_valid()) {
                asset->set_tiling_info(*t);
                return true;
            }

            asset->set_tiling_info(std::nullopt);
            return true;
        } else {
            asset->set_tiling_info(std::nullopt);
            return true;
        }
};

    bool updated_any = false;
    for (Asset* asset : assets_->all) {
        updated_any |= apply_for_asset(asset);
    }
    if (!updated_any && target_valid && current_target) {
        (void)apply_for_asset(current_target);
    }
    if (updated_any && assets_) {
        assets_->mark_active_assets_dirty();
    }
}

bool AssetInfoUI::validate_target_asset() const {
    if (!target_asset_) {
        return false;
    }
    if (!assets_) {
        return true;
    }
    if (!assets_->contains_asset(target_asset_)) {
        target_asset_ = nullptr;
        return false;
    }
    return true;
}

void AssetInfoUI::request_apply_section(AssetInfoSectionId section_id) {
    if (!info_) return;
    if (is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Panel is locked; bulk apply request ignored.");
        return;
    }
    if (!asset_selector_) asset_selector_ = std::make_unique<SearchAssets>();
    if (!asset_selector_) return;

    asset_selector_->open([this, section_id](const std::string& selection) {
        if (selection.empty()) return;
        if (!selection.empty() && selection.front() == '#') return;
        std::string asset_key = resolve_asset_manifest_key(manifest_store_, selection);
        if (asset_key.empty()) {
            SDL_Log("Unable to resolve manifest asset for '%s'", selection.c_str());
            return;
        }
        std::vector<std::string> assets{asset_key};
        (void)apply_section_to_assets(section_id, assets);
    });

    const SDL_Rect& panel = container_.panel_rect();
    if (panel.w > 0) {
        int search_width = 280;
        int search_x = panel.x - search_width - DMSpacing::panel_padding();
        if (search_x < DMSpacing::panel_padding()) search_x = DMSpacing::panel_padding();
        int search_y = panel.y + DMSpacing::panel_padding();
        asset_selector_->set_position(search_x, search_y);
    }
}

bool AssetInfoUI::apply_section_to_assets(AssetInfoSectionId section_id, const std::vector<std::string>& asset_names) {
    if (!info_) return false;
    if (asset_names.empty()) return true;
    if (is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Panel is locked; apply_section_to_assets skipped.");
        return false;
    }

    if (!manifest_store_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Manifest store unavailable; cannot apply settings to other assets.");
        return false;
    }

    nlohmann::json source = info_->manifest_payload();
    bool all_success = true;
    bool any_written = false;
    auto tags_notified = std::make_shared<bool>(false);

    for (const auto& name : asset_names) {
        if (name.empty()) {
            continue;
        }
        std::string target_key = name;
        if (auto resolved = manifest_store_->resolve_asset_name(name)) {
            target_key = *resolved;
        }

        auto apply_fn = [this, section_id, source, target_key](devmode::core::ManifestStore& store) -> bool {
            auto session = store.begin_asset_edit(target_key, false);
            if (!session) {
                SDL_Log("Failed to open manifest session for '%s'", target_key.c_str());
                return false;
            }

            nlohmann::json& target = session.data();
            if (!target.is_object()) {
                target = nlohmann::json::object();
            }
            if (!copy_section_from_source(section_id, source, target)) {
                session.cancel();
                return false;
            }
            return session.commit();
        };

        auto on_success = [this, section_id, tags_notified]() {
            if (section_id == AssetInfoSectionId::Tags) {
                if (!*tags_notified) {
                    tag_utils::notify_tags_changed();
                    *tags_notified = true;
                }
                sync_target_tags();
            }
        };

        if (save_coordinator_) {
            const std::string label = std::string(section_display_name(section_id)) + " -> " + target_key;
            save_coordinator_->enqueue_custom(devmode::core::DevSaveCoordinator::IntentKind::ManifestAsset,
                                              std::string("asset:") + target_key,
                                              apply_fn,
                                              devmode::core::DevSaveCoordinator::Priority::Immediate,
                                              label,
                                              on_success);
            any_written = true;
            continue;
        }

        if (!apply_fn(*manifest_store_)) {
            all_success = false;
        } else {
            any_written = true;
            on_success();
        }
    }

    if (save_coordinator_ && any_written) {
        save_coordinator_->flush_now("Apply section");
    }

    if (any_written) {
        if (!save_coordinator_) {
            if (section_id == AssetInfoSectionId::Tags && !*tags_notified) {
                tag_utils::notify_tags_changed();
                *tags_notified = true;
            }
        }
    }

    if (all_success) {
        pulse_header();
    } else {
        SDL_Log("Some assets failed to receive applied settings.");
    }
    return all_success;
}

void AssetInfoUI::set_header_visibility_callback(std::function<void(bool)> cb) {
    container_.set_header_visibility_controller(std::move(cb));
}

void AssetInfoUI::mark_target_asset_composite_dirty() {
    if (!assets_ || !target_asset_) {
        return;
    }
    target_asset_->mark_composite_dirty();
    assets_->mark_active_assets_dirty();
}

void AssetInfoUI::sync_target_spacing_settings() {
    if (!info_) {
        return;
    }

    bool updated_any = apply_to_assets_with_info([&](Asset* asset) {
        if (!asset->info) {
            return;
        }
        asset->info->set_min_same_type_distance(info_->min_same_type_distance);
        asset->info->set_min_distance_all(info_->min_distance_all);
        asset->info->set_neighbor_search_radius(info_->NeighborSearchRadius);
        asset->NeighborSearchRadius = info_->NeighborSearchRadius;
        asset->clear_grid_residency_cache();
    });

    if (updated_any && assets_) {
        assets_->mark_active_assets_dirty();
    }
}

void AssetInfoUI::sync_target_tags() {
    if (!info_) {
        return;
    }

    bool updated_any = apply_to_assets_with_info([&](Asset* asset) {
        if (!asset->info) {
            return;
        }
        asset->info->set_tags(info_->tags);
        asset->info->set_anti_tags(info_->anti_tags);
    });

    if (updated_any && assets_) {
        assets_->mark_active_assets_dirty();
    }
}

void AssetInfoUI::sync_target_basic_render_settings(bool type_changed) {
    if (!info_) {
        return;
    }

    bool updated_any = apply_to_assets_with_info([&](Asset* asset) {
        if (!asset->info) {
            return;
        }
        asset->info->set_asset_type(info_->type);
        asset->info->set_flipable(info_->flipable);
    });

    if (updated_any && assets_) {
        assets_->mark_active_assets_dirty();
        if (type_changed) {
            assets_->refresh_active_asset_lists();
        }
    }
}

const char* AssetInfoUI::section_display_name(AssetInfoSectionId section_id) {
    switch (section_id) {
        case AssetInfoSectionId::BasicInfo:   return "Basic Info";
        case AssetInfoSectionId::Tags:        return "Tags";
        case AssetInfoSectionId::Spacing:     return "Spacing";
    }
    return "Settings";
}

bool AssetInfoUI::is_point_inside(int x, int y) const {
    if (!visible_) return false;
    SDL_Point p{ x, y };

    if (animation_editor_window_ && animation_editor_window_->is_visible()) {
        if (animation_editor_rect_.w > 0 && animation_editor_rect_.h > 0 &&
            SDL_PointInRect(&p, &animation_editor_rect_)) {
            return true;
        }
    }

    if (container_.is_point_inside(x, y)) return true;
    if (asset_selector_ && asset_selector_->visible() && asset_selector_->is_point_inside(x, y)) return true;
    return false;
}

void AssetInfoUI::save_now() const {
    if (is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Panel is locked; save skipped.");
        return;
    }
    if (info_) {
        auto* self = const_cast<AssetInfoUI*>(this);
        self->enqueue_manifest_save(devmode::core::DevSaveCoordinator::Priority::Immediate, "Asset save");
    }
}

std::shared_ptr<animation_editor::AnimationDocument> AssetInfoUI::animation_document() const {
    if (animation_editor_window_) {
        return animation_editor_window_->document();
    }
    return nullptr;
}
void AssetInfoUI::rebuild_default_sections() {
    sections_.clear();
    section_bounds_.clear();
    basic_info_section_ = nullptr;
    focused_section_ = nullptr;

    auto adopt_section = [](auto* section) {
        configure_panel_for_container(section);
};

    auto finalize_section = [this](DockableCollapsible* section) {
        if (!section) {
            return;
        }
        section->set_info(info_);
        section->reset_scroll();
        section->set_expanded(false);
        try {
            section->build();
        } catch (const std::exception& ex) {
            SDL_Log("AssetInfoUI: failed to build section during initialization: %s", ex.what());
        } catch (...) {
            SDL_Log("AssetInfoUI: failed to build section during initialization due to unknown error.");
        }
};

    auto basic = std::make_unique<Section_BasicInfo>();
    basic->set_ui(this);
    basic_info_section_ = basic.get();
    adopt_section(basic_info_section_);
    finalize_section(basic_info_section_);
    sections_.push_back(std::move(basic));

    auto tags = std::make_unique<Section_Tags>();
    tags->set_ui(this);
    adopt_section(tags.get());
    finalize_section(tags.get());
    sections_.push_back(std::move(tags));

    auto spacing = std::make_unique<Section_Spacing>();
    spacing->set_ui(this);
    adopt_section(spacing.get());
    finalize_section(spacing.get());
    sections_.push_back(std::move(spacing));

    container_.reset_scroll();
    container_.request_layout();
    clear_section_focus();
}

bool AssetInfoUI::apply_to_assets_with_info(const std::function<void(Asset*)>& fn) {
    if (!info_) {
        return false;
    }

    std::unordered_set<Asset*> visited;
    auto visit = [&](Asset* asset) {
        if (!asset_matches_current_info(asset)) {
            return;
        }
        if (!visited.insert(asset).second) {
            return;
        }
        fn(asset);
};

    if (assets_) {
        for (Asset* asset : assets_->all) {
            visit(asset);
        }
    }
    visit(target_asset_);
    return !visited.empty();
}

bool AssetInfoUI::asset_matches_current_info(const Asset* asset) const {
    if (!info_ || !asset || !asset->info) {
        return false;
    }
    if (asset->info.get() == info_.get()) {
        return true;
    }
    if (!info_->name.empty() && asset->info->name == info_->name) {
        return true;
    }
    if (!info_->asset_dir_path().empty() && asset->info->asset_dir_path() == info_->asset_dir_path()) {
        return true;
    }
    return false;
}

void AssetInfoUI::refresh_loaded_asset_instances() {
    if (!info_) {
        return;
    }

    if (!info_->name.empty()) {

    }

    if (!info_->name.empty()) {

        render_pipeline::ScalingLogic::LoadPrecomputedProfiles(true);
        auto profile = render_pipeline::ScalingLogic::ProfileForAsset(info_->name);
        (void)profile;
    }

    if (assets_) {
        devmode::refresh_loaded_animation_instances(assets_, info_);
    }

    if (assets_ && !info_->name.empty()) {
        for (auto& [lib_name, lib_info] : assets_->library().all()) {
            if (!lib_info || lib_name == info_->name) continue;

            bool needs_refresh = false;
            for (const auto& [anim_id, anim_data] : lib_info->animations) {
                if (anim_data.source.kind == "animation" && anim_data.source.path == info_->name) {
                    needs_refresh = true;
                    break;
                }
            }

            if (!needs_refresh) {
                continue;
            }

            devmode::refresh_loaded_animation_instances(assets_, lib_info);
        }
    }
}

void AssetInfoUI::on_animation_document_saved() {
    if (!info_) {
        return;
    }

    const nlohmann::json before_snapshot = animation_manifest_snapshot(*info_);
    const bool reloaded = info_->reload_animations_from_disk();
    if (!reloaded) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to reload animations for %s.", info_->name.c_str());
    }
    const nlohmann::json after_snapshot = animation_manifest_snapshot(*info_);
    const bool animation_data_changed = reloaded && before_snapshot != after_snapshot;

    if (animation_data_changed) {
        info_->classify_animation_snapshot_rebuilds(before_snapshot, after_snapshot);
        info_->mark_dirty();
    }

    SDL_Renderer* renderer = nullptr;
    if (assets_) {
        renderer = assets_->renderer();
    }
    if (!renderer) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] No renderer available for animation reload");
        return;
    }
    if (!reloaded) {
        return;
    }

    info_->loadAnimations(renderer);
    refresh_loaded_asset_instances();
}

bool AssetInfoUI::duplicate_current_asset(const std::string& raw_name) {
    if (!info_) return false;
    std::string name = devmode::utils::normalize_asset_name(raw_name);
    if (name.empty()) return false;
    if (!manifest_store_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Manifest store unavailable; cannot duplicate '%s' to '%s'", info_->name.c_str(), name.c_str());
        return false;
    }

    auto session = manifest_store_->begin_asset_edit(name, true);
    if (!session) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to begin manifest session for '%s'", name.c_str());
        return false;
    }
    if (!session.is_new_asset()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Asset '%s' already exists", name.c_str());
        session.cancel();
        return false;
    }

    namespace fs = std::filesystem;
    fs::path base = asset_paths::assets_root_path();
    fs::path src_dir;
    try {
        const std::string src_dir_str = info_->asset_dir_path();
        if (!src_dir_str.empty()) src_dir = fs::path(src_dir_str);
        if (src_dir.empty()) src_dir = base / info_->name;
    } catch (...) {
        src_dir.clear();
    }
    fs::path dst_dir = base / name;

    try {
        if (!fs::exists(base)) {
            fs::create_directories(base);
        }
        if (fs::exists(dst_dir)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Destination directory '%s' already exists", dst_dir.string().c_str());
            session.cancel();
            return false;
        }
        fs::create_directories(dst_dir);

        std::error_code ec;
        if (!src_dir.empty() && fs::exists(src_dir, ec)) {
            fs::copy(src_dir, dst_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Some files failed to copy from '%s' to '%s': %s", src_dir.string().c_str(), dst_dir.string().c_str(), ec.message().c_str());
            }
        }

        nlohmann::json manifest_entry;
        if (manifest_store_) {
            auto view = manifest_store_->get_asset(info_->name);
            if (view && view.data) {
                manifest_entry = *view.data;
            }
        }
        if (!manifest_entry.is_object()) manifest_entry = nlohmann::json::object();

        const std::string dst_dir_str = dst_dir.lexically_normal().generic_string();
        manifest_entry["asset_name"] = name;
        manifest_entry["asset_directory"] = dst_dir_str;

        manifest_entry["start"] = dst_dir_str;

        session.data() = manifest_entry;
        if (!session.commit()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to commit manifest entry for '%s'", name.c_str());
            std::error_code cleanup_ec;
            fs::remove_all(dst_dir, cleanup_ec);
            return false;
        }

        if (assets_) {
            assets_->library().add_asset(name, manifest_entry);
            if (SDL_Renderer* renderer = assets_->renderer()) {
                assets_->library().ensureAnimationsLoadedFor(renderer, std::unordered_set<std::string>{name});
            }
            assets_->show_dev_notice(std::string("Duplicated asset as '") + name + "'");
        }
        return true;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Exception duplicating asset '%s' -> '%s': %s", info_->name.c_str(), name.c_str(), e.what());
        std::error_code cleanup_ec;
        fs::remove_all(dst_dir, cleanup_ec);
        return false;
    }
}

void AssetInfoUI::request_delete_current_asset() {
    if (!info_) return;
    PendingDeleteInfo pending;
    pending.name = info_->name;
    pending.asset_dir = info_->asset_dir_path();
    if (pending.asset_dir.empty() && !info_->name.empty()) {
        pending.asset_dir = asset_paths::asset_folder_path(info_->name).generic_string();
    }
    pending_delete_ = std::move(pending);
    showing_delete_popup_ = true;
    delete_yes_hovered_ = delete_no_hovered_ = false;
    delete_yes_pressed_ = delete_no_pressed_ = false;
}

void AssetInfoUI::cancel_delete_request() {
    showing_delete_popup_ = false;
    clear_delete_state();
}

void AssetInfoUI::confirm_delete_request() {
    if (!pending_delete_) {
        clear_delete_state();
        showing_delete_popup_ = false;
        return;
    }

    const PendingDeleteInfo pending = *pending_delete_;
    const std::string asset_name = pending.name;
    const std::filesystem::path asset_dir = pending.asset_dir.empty() ? asset_paths::asset_folder_path(asset_name) : std::filesystem::path(pending.asset_dir);
    const std::filesystem::path cache_dir = std::filesystem::path("cache") / asset_name;

    showing_delete_popup_ = false;

    if (assets_) {
        assets_->clear_editor_selection();
        std::vector<Asset*> doomed;
        doomed.reserve(assets_->all.size());
        for (Asset* asset : assets_->all) {
            if (!asset || !asset->info) continue;
            if (asset->info->name == asset_name) {
                doomed.push_back(asset);
            }
        }
        for (Asset* asset : doomed) {
            asset->Delete();
        }
    }

    bool manifest_entry_removed = false;
    if (!asset_name.empty()) {
        if (manifest_store_) {
            const auto remove_result = devmode::manifest_utils::remove_asset_entry(manifest_store_, asset_name, &std::cerr);
            manifest_entry_removed = remove_result.removed;
            if (!manifest_entry_removed) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to remove '%s' from manifest", asset_name.c_str());
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Manifest store unavailable; manifest not updated for '%s'", asset_name.c_str());
            manifest_entry_removed = devmode::manifest_utils::remove_manifest_asset_entry(asset_name, &std::cerr);
            if (!manifest_entry_removed) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Failed to remove '%s' from manifest assets list", asset_name.c_str());
            }
        }
    }

    auto remove_directory_if_exists = [](const std::filesystem::path& path) {
        std::error_code ec;
        if (path.empty()) return true;
        if (!std::filesystem::exists(path, ec)) return true;
        std::filesystem::remove_all(path, ec);
        return !ec;
};

    if (!asset_dir.empty()) {
        const auto normalized_dir = asset_dir.lexically_normal();
        if (asset_paths::is_protected_asset_root(normalized_dir)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AssetInfoUI] Refusing to remove protected asset root '%s'", normalized_dir.generic_string().c_str());
        } else {
            remove_directory_if_exists(normalized_dir);
        }
    }
    if (!asset_name.empty()) {
        remove_directory_if_exists(cache_dir);
    }


    if (assets_ && !asset_name.empty()) {
        assets_->library().remove(asset_name);
    }

    if (info_ && info_->name == asset_name) {
        clear_info();
        close();
    }

    clear_delete_state();
}

void AssetInfoUI::clear_delete_state() {
    pending_delete_.reset();
    delete_yes_hovered_ = delete_no_hovered_ = false;
    delete_yes_pressed_ = delete_no_pressed_ = false;
    delete_modal_rect_ = SDL_Rect{0, 0, 0, 0};
    delete_yes_rect_ = SDL_Rect{0, 0, 0, 0};
    delete_no_rect_ = SDL_Rect{0, 0, 0, 0};
}

void AssetInfoUI::update_delete_modal_geometry(int screen_w, int screen_h) {
    const int modal_w = 420;
    const int modal_h = 160;
    delete_modal_rect_ = SDL_Rect{
        std::max(0, screen_w / 2 - modal_w / 2), std::max(0, screen_h / 2 - modal_h / 2), modal_w, modal_h };
    const int button_w = 140;
    const int button_h = 40;
    const int button_gap = 20;
    const int total_w = button_w * 2 + button_gap;
    const int buttons_x = delete_modal_rect_.x + (delete_modal_rect_.w - total_w) / 2;
    const int buttons_y = delete_modal_rect_.y + delete_modal_rect_.h - button_h - 20;
    delete_yes_rect_ = SDL_Rect{ buttons_x, buttons_y, button_w, button_h };
    delete_no_rect_ = SDL_Rect{ buttons_x + button_w + button_gap, buttons_y, button_w, button_h };
}

bool AssetInfoUI::handle_delete_modal_event(const SDL_Event& e) {
    if (!showing_delete_popup_) return false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p{ static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y)) };
        delete_yes_hovered_ = SDL_PointInRect(&p, &delete_yes_rect_);
        delete_no_hovered_ = SDL_PointInRect(&p, &delete_no_rect_);
        return SDL_PointInRect(&p, &delete_modal_rect_);
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p{ static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y)) };
        if (SDL_PointInRect(&p, &delete_yes_rect_)) { delete_yes_pressed_ = true; return true; }
        if (SDL_PointInRect(&p, &delete_no_rect_)) { delete_no_pressed_ = true; return true; }
        if (SDL_PointInRect(&p, &delete_modal_rect_)) return true;
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p{ static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y)) };
        const bool inside_yes = SDL_PointInRect(&p, &delete_yes_rect_);
        const bool inside_no  = SDL_PointInRect(&p, &delete_no_rect_);
        bool consumed = SDL_PointInRect(&p, &delete_modal_rect_);
        if (inside_yes && delete_yes_pressed_) { delete_yes_pressed_ = false; delete_no_pressed_ = false; confirm_delete_request(); return true; }
        if (inside_no  && delete_no_pressed_)  { delete_yes_pressed_ = false; delete_no_pressed_ = false; cancel_delete_request();  return true; }
        delete_yes_pressed_ = false; delete_no_pressed_ = false; return consumed;
    }
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_Y || e.key.key == SDLK_SPACE) { confirm_delete_request(); return true; }
        if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_N) { cancel_delete_request(); return true; }
        return true;
    }
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        return true;
    }
    return false;
}




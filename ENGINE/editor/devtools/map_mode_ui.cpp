#include "map_mode_ui.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include "DockableCollapsible.hpp"
#include "DockManager.hpp"
#include "dev_footer_bar.hpp"
#include "map_layers_controller.hpp"
#include "map_layer_controls_display.hpp"
#include "map_layers_common.hpp"
#include "map_layers_panel.hpp"
#include "map_rooms_display.hpp"
#include "config/room_config/room_configurator.hpp"
#include "spawn_groups/spawn_group_utils.hpp"
#include "SlidingWindowContainer.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/widgets.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/core/save_manager.hpp"
#include "dm_styles.hpp"
#include "utils/input.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <iostream>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

class MapLayersPreviewPanel : public DockableCollapsible {
public:
    using SaveCallback = std::function<bool()>;

    explicit MapLayersPreviewPanel(int x = 72, int y = 40)
        : DockableCollapsible("Layers Preview", true, x, y) {
        build_rows();
        set_visible(false);
        set_expanded(true);
    }

    ~MapLayersPreviewPanel() override = default;

    void set_map_info(nlohmann::json* map_info, SaveCallback on_save = nullptr) {
        map_info_ = map_info;
        on_save_ = std::move(on_save);
        if (preview_widget_) {
            preview_widget_->set_map_info(map_info_);
            preview_widget_->set_on_change([this]() { this->trigger_save(); });
        }
    }

    void set_controller(std::shared_ptr<MapLayersController> controller) {
        controller_ = std::move(controller);
        if (preview_widget_) {
            preview_widget_->set_controller(controller_);
        }
    }

    void set_on_select_layer(std::function<void(int)> cb) {
        on_select_layer_ = std::move(cb);
        if (preview_widget_) {
            preview_widget_->set_on_select_layer(on_select_layer_);
        }
    }

    void set_on_select_room(std::function<void(const std::string&)> cb) {
        on_select_room_ = std::move(cb);
        if (preview_widget_) {
            preview_widget_->set_on_select_room(on_select_room_);
        }
    }

    void set_on_show_room_list(std::function<void()> cb) {
        on_show_room_list_ = std::move(cb);
        if (preview_widget_) {
            preview_widget_->set_on_show_room_list(on_show_room_list_);
        }
    }

    void update(const Input& input, int screen_w = 0, int screen_h = 0) override {
        DockableCollapsible::update(input, screen_w, screen_h);
    }

    bool handle_event(const SDL_Event& e) override {
        if (!is_visible()) {
            return false;
        }
        return DockableCollapsible::handle_event(e);
    }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer) {
            return;
        }
        DockableCollapsible::render(renderer);
    }

    bool is_point_inside(int x, int y) const override {
        return DockableCollapsible::is_point_inside(x, y);
    }

private:
    void build_rows() {
        owned_widgets_.clear();
        preview_widget_ = nullptr;

        if (!add_layer_btn_) {
            add_layer_btn_ = std::make_unique<DMButton>("Add Layer", &DMStyles::CreateButton(), 0, DMButton::height());
        }
        if (!create_room_btn_) {
            create_room_btn_ = std::make_unique<DMButton>("Create Room", &DMStyles::CreateButton(), 0, DMButton::height());
        }
        if (!reload_btn_) {
            reload_btn_ = std::make_unique<DMButton>("Reload", &DMStyles::ListButton(), 0, DMButton::height());
        }

        std::vector<Widget*> button_row;
        owned_widgets_.push_back(std::make_unique<ButtonWidget>(add_layer_btn_.get(), [this]() {
            if (controller_) {
                const int created = controller_->create_layer();
                if (preview_widget_) {
                    preview_widget_->mark_dirty();
                }
                if (created >= 0) {
                    trigger_save();
                }
            }
        }));
        button_row.push_back(owned_widgets_.back().get());

        owned_widgets_.push_back(std::make_unique<ButtonWidget>(create_room_btn_.get(), [this]() {
            if (preview_widget_) {
                preview_widget_->create_new_room_entry();
            }
            trigger_save();
        }));
        button_row.push_back(owned_widgets_.back().get());

        owned_widgets_.push_back(std::make_unique<ButtonWidget>(reload_btn_.get(), [this]() {
            if (controller_) {
                controller_->reload();
                if (preview_widget_) {
                    preview_widget_->mark_dirty();
                }
            }
        }));
        button_row.push_back(owned_widgets_.back().get());

        auto preview = std::make_unique<MapLayersPreviewWidget>();
        preview->set_map_info(map_info_);
        preview->set_controller(controller_);
        preview->set_on_select_layer(on_select_layer_);
        preview->set_on_select_room(on_select_room_);
        preview->set_on_show_room_list(on_show_room_list_);
        preview->set_on_change([this]() { this->trigger_save(); });

        owned_widgets_.push_back(std::move(preview));
        preview_widget_ = static_cast<MapLayersPreviewWidget*>(owned_widgets_.back().get());

        Rows rows;
        rows.push_back(button_row);
        rows.push_back(Row{preview_widget_});
        set_rows(rows);
    }

    void trigger_save() {
        bool ok = false;
        if (controller_) {
            ok = controller_->save();
        }
        if (!ok && on_save_) {
            ok = on_save_();
        }
        (void)ok;
    }

private:
    nlohmann::json* map_info_ = nullptr;
    SaveCallback on_save_{};
    std::shared_ptr<MapLayersController> controller_;

    std::vector<std::unique_ptr<Widget>> owned_widgets_;
    MapLayersPreviewWidget* preview_widget_ = nullptr;

    std::unique_ptr<DMButton> add_layer_btn_;
    std::unique_ptr<DMButton> create_room_btn_;
    std::unique_ptr<DMButton> reload_btn_;

    std::function<void(int)> on_select_layer_{};
    std::function<void(const std::string&)> on_select_room_{};
    std::function<void()> on_show_room_list_{};
};

namespace {
constexpr int kDefaultPanelX = 48;
constexpr int kDefaultPanelY = 48;

std::string trim_copy(const std::string& input) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    std::string result = input;
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [&](unsigned char ch) {
        return !is_space(ch);
    }));
    result.erase(std::find_if(result.rbegin(), result.rend(), [&](unsigned char ch) {
        return !is_space(ch);
    }).base(), result.end());
    return result;
}

using devmode::spawn::ensure_spawn_group_entry_defaults;
using devmode::spawn::ensure_spawn_groups_array;
using devmode::spawn::generate_spawn_id;
using devmode::spawn::sanitize_perimeter_spawn_groups;

std::string sanitize_room_key(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_underscore = false;
    for (char ch : input) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            out.push_back(static_cast<char>(std::tolower(uch)));
            last_underscore = false;
        } else if (ch == '_' || ch == '-') {
            if (!last_underscore && !out.empty()) {
                out.push_back('_');
                last_underscore = true;
            }
        } else if (std::isspace(uch)) {
            if (!last_underscore && !out.empty()) {
                out.push_back('_');
                last_underscore = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "room";
    }
    return out;
}

}

MapModeUI::MapModeUI(Assets* assets)
    : assets_(assets) {}

MapModeUI::~MapModeUI() {
    cancel_map_color_sampling(true);
    if (map_color_sampling_cursor_handle_) {
        SDL_DestroyCursor(map_color_sampling_cursor_handle_);
        map_color_sampling_cursor_handle_ = nullptr;
    }
}

void MapModeUI::set_manifest_store(devmode::core::ManifestStore* store) {
    SDL_assert(store != nullptr);
    manifest_store_ = store;
    if (layers_controller_) {
        layers_controller_->set_manifest_store(manifest_store_, map_id_);
        layers_controller_->set_save_coordinator(save_coordinator_);
        layers_controller_->set_save_manager(save_manager_);
    }
}

void MapModeUI::set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) {
    save_coordinator_ = coordinator;
    if (layers_controller_) {
        layers_controller_->set_save_coordinator(save_coordinator_);
        layers_controller_->set_save_manager(save_manager_);
    }
}

void MapModeUI::set_save_manager(devmode::core::SaveManager* manager) {
    save_manager_ = manager;
    if (layers_controller_) {
        layers_controller_->set_save_manager(save_manager_);
    }
}

void MapModeUI::mark_layers_clean() {
    if (layers_controller_) {
        layers_controller_->mark_clean();
    }
    if (layers_panel_) {
        layers_panel_->mark_dirty(false);
    }
}

void MapModeUI::notify_saved() {
    if (on_saved_) {
        on_saved_();
    }
}

void MapModeUI::set_map_context(nlohmann::json* map_info, const std::string& map_path) {
    map_info_ = map_info;
    map_path_ = map_path;
    map_id_ = assets_ ? assets_->map_id() : std::string{};
    if (layers_controller_) {
        layers_controller_->set_manifest_store(manifest_store_, map_id_);
        layers_controller_->set_save_manager(save_manager_);
    }
    sync_panel_map_info();
}

void MapModeUI::set_screen_dimensions(int w, int h) {
    screen_w_ = w;
    screen_h_ = h;
    ensure_panels();
    sliding_area_bounds_ = sanitize_sliding_area(sliding_area_bounds_);
    apply_sliding_area_bounds();
    update_footer_visibility();
}

void MapModeUI::set_sliding_area_bounds(const SDL_Rect& bounds) {
    SDL_Rect sanitized = sanitize_sliding_area(bounds);
    if (sanitized.x == sliding_area_bounds_.x &&
        sanitized.y == sliding_area_bounds_.y &&
        sanitized.w == sliding_area_bounds_.w &&
        sanitized.h == sliding_area_bounds_.h) {
        return;
    }
    sliding_area_bounds_ = sanitized;
    ensure_panels();
    apply_sliding_area_bounds();
}

void MapModeUI::set_map_mode_active(bool active) {
    map_mode_active_ = active;
    if (active) {
        footer_buttons_configured_ = false;
    }
    ensure_panels();
    update_footer_visibility();
    sync_footer_button_states();
    set_active_panel(PanelType::None);
    if (!active) {
        close_room_configuration(false);
    }
}

DevFooterBar* MapModeUI::get_footer_bar() const {
    return footer_bar_.get();
}

void MapModeUI::collect_sliding_container_rects(std::vector<SDL_Rect>& out) const {
    auto append_container = [&out](const SlidingWindowContainer* container) {
        if (!container || !container->is_visible()) {
            return;
        }
        const SDL_Rect& rect = container->panel_rect();
        if (rect.w > 0 && rect.h > 0) {
            out.push_back(rect);
        }
};

    append_container(room_config_container_.get());
    append_container(rooms_list_container_.get());
    append_container(layer_controls_container_.get());

    if (room_configurator_ && room_configurator_->visible()) {
        const SDL_Rect& rect = room_configurator_->panel_rect();
        if (rect.w > 0 && rect.h > 0) {
            out.push_back(rect);
        }
    }
}

SDL_Rect MapModeUI::sanitize_sliding_area(const SDL_Rect& bounds) const {
    if (screen_w_ <= 0 || screen_h_ <= 0) {
        return SDL_Rect{0, 0, 0, 0};
    }
    SDL_Rect result = bounds;
    if (result.w <= 0 || result.h <= 0) {
        result = SDL_Rect{0, 0, screen_w_, screen_h_};
    }
    if (result.w > screen_w_) {
        result.w = screen_w_;
    }
    if (result.h > screen_h_) {
        result.h = screen_h_;
    }
    int max_x = std::max(0, screen_w_ - result.w);
    int max_y = std::max(0, screen_h_ - result.h);
    result.x = std::clamp(result.x, 0, max_x);
    result.y = std::clamp(result.y, 0, max_y);
    return result;
}

SDL_Rect MapModeUI::effective_work_area() const {
    if (screen_w_ <= 0 || screen_h_ <= 0) {
        return SDL_Rect{0, 0, 0, 0};
    }
    SDL_Rect area = sliding_area_bounds_;
    if (area.w <= 0 || area.h <= 0) {
        return SDL_Rect{0, 0, screen_w_, screen_h_};
    }
    int height = std::min(area.h, screen_h_);
    int y = std::clamp(area.y, 0, std::max(0, screen_h_ - height));
    return SDL_Rect{0, y, screen_w_, height};
}

void MapModeUI::apply_sliding_area_bounds() {
    sliding_area_bounds_ = sanitize_sliding_area(sliding_area_bounds_);
    SDL_Rect work_area = effective_work_area();
    SDL_Rect right_bounds = room_config_bounds();

    if (layers_preview_panel_) layers_preview_panel_->set_work_area(work_area);

    if (layers_panel_) {
        layers_panel_->set_work_area(work_area);
        int left_width = work_area.w;
        if (right_bounds.w > 0) {
            int available = right_bounds.x - work_area.x;
            left_width = std::clamp(available, 0, work_area.w);
        }
        SDL_Rect left_bounds{work_area.x, work_area.y, left_width, work_area.h};
        layers_panel_->set_embedded_bounds(left_bounds);
    }

    if (room_configurator_) {
        room_configurator_->set_work_area(work_area);
        room_configurator_->set_bounds(right_bounds);
    }
    if (room_config_container_) {
        room_config_container_->set_panel_bounds_override(right_bounds);
    }
    if (rooms_list_container_) {
        rooms_list_container_->set_panel_bounds_override(right_bounds);
    }
    if (layer_controls_container_) {
        layer_controls_container_->set_panel_bounds_override(right_bounds);
    }
}

void MapModeUI::set_footer_always_visible(bool on) {
    footer_always_visible_ = on;
    ensure_panels();
    update_footer_visibility();
}

void MapModeUI::set_headers_suppressed(bool suppressed) {
    base_headers_suppressed_ = suppressed;
    refresh_header_suppression_state();
}

void MapModeUI::set_sliding_headers_hidden(bool hidden) {
    int previous = sliding_header_request_count_;
    if (hidden) {
        ++sliding_header_request_count_;
    } else if (sliding_header_request_count_ > 0) {
        --sliding_header_request_count_;
    }
    if (previous == sliding_header_request_count_) {
        return;
    }
    refresh_header_suppression_state();
}

void MapModeUI::set_dev_sliding_headers_hidden(bool hidden) {
    if (dev_sliding_headers_hidden_ == hidden) {
        return;
    }
    dev_sliding_headers_hidden_ = hidden;
    refresh_header_suppression_state();
}

void MapModeUI::refresh_header_suppression_state() {
    const bool sliding_requested = (sliding_header_request_count_ > 0) || dev_sliding_headers_hidden_;
    const bool final_state = base_headers_suppressed_ || sliding_requested;
    const bool sliding_only = sliding_requested && !base_headers_suppressed_;
    const bool state_changed = (headers_suppressed_ != final_state) || (sliding_only_header_suppression_ != sliding_only);
    headers_suppressed_ = final_state;
    sliding_only_header_suppression_ = sliding_only;

    if (state_changed) {
        ensure_panels();
        if (headers_suppressed_ && !sliding_only_header_suppression_) {
            if (layers_panel_) {
                layers_panel_->close();
            }
            close_room_configuration(false);
        }
    }

    update_footer_visibility();
}

void MapModeUI::set_mode_button_sets(std::vector<HeaderButtonConfig> map_buttons,
                                     std::vector<HeaderButtonConfig> room_buttons) {
    map_mode_buttons_ = std::move(map_buttons);
    room_mode_buttons_ = std::move(room_buttons);
    footer_buttons_configured_ = false;
    ensure_panels();
}

void MapModeUI::set_header_mode(HeaderMode mode) {
    if (header_mode_ == mode) {
        return;
    }
    header_mode_ = mode;
    footer_buttons_configured_ = false;
    ensure_panels();
    sync_footer_button_states();
}

MapModeUI::HeaderButtonConfig* MapModeUI::find_button(HeaderMode mode, const std::string& id) {
    auto& list = (mode == HeaderMode::Map) ? map_mode_buttons_ : room_mode_buttons_;
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const HeaderButtonConfig& cfg) { return cfg.id == id; });
    if (it == list.end()) {
        return nullptr;
    }
    return &(*it);
}

bool MapModeUI::ensure_panel_unlocked(DockableCollapsible* panel, const char* panel_name) const {
    if (panel && panel->isLocked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[MapModeUI] %s panel is locked; action ignored.", panel_name);
        return false;
    }
    return true;
}

void MapModeUI::set_button_state(const std::string& id, bool active) {
    set_button_state(header_mode_, id, active);
}

void MapModeUI::set_button_state(HeaderMode mode, const std::string& id, bool active) {
    if (HeaderButtonConfig* cfg = find_button(mode, id)) {
        cfg->active = active;
    }
    if (footer_bar_ && mode == header_mode_) {
        footer_bar_->set_button_active_state(id, active);
    }
}

void MapModeUI::register_floating_panel(DockableCollapsible* panel) {
    track_floating_panel(panel);
}

void MapModeUI::track_floating_panel(DockableCollapsible* panel) {
    if (!panel) return;
    auto it = std::find(floating_panels_.begin(), floating_panels_.end(), panel);
    if (it == floating_panels_.end()) {
        floating_panels_.push_back(panel);
    }
}

void MapModeUI::rebuild_floating_stack() {
    floating_panels_.erase( std::remove(floating_panels_.begin(), floating_panels_.end(), nullptr), floating_panels_.end());
}

void MapModeUI::bring_panel_to_front(DockableCollapsible* panel) {
    if (!panel) return;
    auto it = std::find(floating_panels_.begin(), floating_panels_.end(), panel);
    if (it == floating_panels_.end()) return;
    if (std::next(it) == floating_panels_.end()) return;
    DockableCollapsible* ptr = *it;
    floating_panels_.erase(it);
    floating_panels_.push_back(ptr);
}

bool MapModeUI::is_pointer_event(const SDL_Event& e) const {
    return e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION;
}

SDL_Point MapModeUI::event_point(const SDL_Event& e) const {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        return SDL_Point{
            static_cast<int>(std::lround(e.motion.x)),
            static_cast<int>(std::lround(e.motion.y))};
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        return SDL_Point{
            static_cast<int>(std::lround(e.button.x)),
            static_cast<int>(std::lround(e.button.y))};
    }
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    return SDL_Point{mx, my};
}

bool MapModeUI::pointer_inside_floating_panel(int x, int y) const {
    SDL_Point p{x, y};
    for (DockableCollapsible* panel : floating_panels_) {
        if (!panel) continue;
        if (auto* layers_preview = dynamic_cast<MapLayersPreviewPanel*>(panel)) {
            if (layers_preview->is_visible() && layers_preview->is_point_inside(p.x, p.y)) {
                return true;
            }
            continue;
        }
        if (panel->is_visible() && panel->is_point_inside(p.x, p.y)) {
            return true;
        }
    }
    for (DockableCollapsible* panel : DockManager::instance().open_panels()) {
        if (!panel || !panel->is_visible()) {
            continue;
        }
        if (panel->is_point_inside(p.x, p.y)) {
            return true;
        }
    }
    return false;
}

bool MapModeUI::handle_floating_panel_event(const SDL_Event& e, bool& used) {
    if (floating_panels_.empty()) return false;

    const bool pointer_event = is_pointer_event(e);
    const bool wheel_event = (e.type == SDL_EVENT_MOUSE_WHEEL);
    SDL_Point p = event_point(e);
    bool consumed = false;

    for (auto it = floating_panels_.rbegin(); it != floating_panels_.rend(); ++it) {
        DockableCollapsible* panel = *it;
        if (!panel) continue;

        MapLayersPreviewPanel* layers_preview = dynamic_cast<MapLayersPreviewPanel*>(panel);

        auto handle_and_check = [&](auto* concrete) -> bool {
            if (!concrete || !concrete->is_visible()) return false;
            if (concrete->handle_event(e)) {
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    bring_panel_to_front(panel);
                }
                used = true;
                return true;
            }
            return false;
};

        bool handled_special = false;
        if (layers_preview) {
            handled_special = handle_and_check(layers_preview);
        } else {
            if (!panel->is_visible()) continue;
            if (panel->handle_event(e)) {
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    bring_panel_to_front(panel);
                }
                used = true;
                consumed = true;
                break;
            }
        }

        if (handled_special) {
            consumed = true;
            break;
        }

        const bool inside_preview = layers_preview && layers_preview->is_visible() && layers_preview->is_point_inside(p.x, p.y);
        const bool inside_panel = panel->is_visible() && panel->is_point_inside(p.x, p.y);
        const bool inside = inside_preview || inside_panel;

        if ((pointer_event || wheel_event) && inside) {
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                bring_panel_to_front(panel);
            }
            used = true;
            consumed = true;
            break;
        }
    }

    if (!consumed && (pointer_event || wheel_event)) {
        for (DockableCollapsible* panel : DockManager::instance().open_panels()) {
            if (!panel || !panel->is_visible()) {
                continue;
            }
            if (panel->is_point_inside(p.x, p.y)) {
                used = true;
                consumed = true;
                break;
            }
        }
    }

    return consumed;
}

void MapModeUI::ensure_panels() {
    if (!layers_controller_) {
        layers_controller_ = std::make_shared<MapLayersController>();
    }
    if (layers_controller_) {
        layers_controller_->set_manifest_store(manifest_store_, map_id_);
        layers_controller_->set_save_coordinator(save_coordinator_);
        layers_controller_->set_save_manager(save_manager_);
        layers_controller_->set_dirty_callback([this](devmode::core::DevSaveCoordinator::Priority priority) {
            if (dirty_callback_) dirty_callback_(priority);
        });
    }
    if (!layers_panel_) {
        layers_panel_ = std::make_unique<MapLayersPanel>();
        layers_panel_->set_embedded_mode(true);
        layers_panel_->set_on_configure_room([this](const std::string& key) {
            this->open_room_configuration(key, SlidingPanel::LayerControls);
        });
        layers_panel_->set_side_panel_callback([this](MapLayersPanel::SidePanel panel) {
            SlidingPanel desired_panel = SlidingPanel::RoomsList;
            switch (panel) {
                case MapLayersPanel::SidePanel::LayerControls:
                    desired_panel = SlidingPanel::LayerControls;
                    break;
                case MapLayersPanel::SidePanel::RoomsList:
                case MapLayersPanel::SidePanel::None:
                default:
                    desired_panel = SlidingPanel::RoomsList;
                    break;
            }

            const bool room_config_open = room_configurator_ && room_configurator_->visible();
            if (room_config_open) {
                if (room_config_return_panel_ != desired_panel) {
                    room_config_return_panel_ = desired_panel;
                    update_room_config_header_controls();
                }
                return;
            }

            this->show_sliding_panel(desired_panel);
        });
        layers_panel_->set_on_close([this]() {

            if (rooms_list_container_) {
                rooms_list_container_->close();
            }
            if (layer_controls_container_) {
                layer_controls_container_->close();
            }
            this->close_room_configuration(false);
            active_panel_ = PanelType::None;
            this->set_sliding_headers_hidden(false);
            this->update_footer_visibility();
            sync_footer_button_states();
        });
    }
    if (layers_panel_) {
        layers_panel_->set_embedded_mode(true);
        layers_panel_->set_header_visibility_callback([this](bool visible) {
            this->set_sliding_headers_hidden(visible);
        });
        if (layers_controller_) {
            layers_panel_->set_controller(layers_controller_);
        }
    }
    if (layers_panel_) {
        floating_panels_.erase(std::remove(floating_panels_.begin(), floating_panels_.end(), layers_panel_.get()), floating_panels_.end());
    }

    if (!rooms_list_container_) {
        rooms_list_container_ = std::make_unique<SlidingWindowContainer>();

        rooms_list_container_->set_header_visibility_controller([this](bool visible) {
            this->set_dev_sliding_headers_hidden(visible);
        });
    }
    if (!rooms_display_) {
        rooms_display_ = std::make_unique<MapRoomsDisplay>();
        rooms_display_->set_header_text("Room List");
        rooms_display_->set_on_select_room([this](const std::string& key) {
            this->open_room_configuration(key, SlidingPanel::RoomsList);
        });
        rooms_display_->set_on_rooms_changed([this]() {
            this->auto_save_layers_data();
        });
    }
    if (rooms_display_) {
        rooms_display_->attach_container(rooms_list_container_.get());
        rooms_display_->set_map_info(map_info_);
        rooms_display_->set_on_rooms_changed([this]() {
            this->auto_save_layers_data();
        });
        rooms_display_->set_on_create_room([this]() {
            this->create_room_from_panel(SlidingPanel::RoomsList);
        });
    }
    if (!layer_controls_container_) {
        layer_controls_container_ = std::make_unique<SlidingWindowContainer>();

        layer_controls_container_->set_header_visibility_controller([this](bool visible) {
            this->set_dev_sliding_headers_hidden(visible);
        });
    }
    if (!layer_controls_display_) {
        layer_controls_display_ = std::make_unique<MapLayerControlsDisplay>();
    }
    if (layer_controls_display_) {
        layer_controls_display_->attach_container(layer_controls_container_.get());
        layer_controls_display_->set_controller(layers_controller_);
        layer_controls_display_->set_selected_layer(layers_panel_ ? layers_panel_->selected_layer() : -1);
        layer_controls_display_->set_on_change([this]() {
            this->auto_save_layers_data();
        });
        layer_controls_display_->set_on_show_rooms_list([this]() {
            this->show_sliding_panel(SlidingPanel::RoomsList);
        });
        layer_controls_display_->set_on_create_room([this]() { this->create_room_from_layers_controls(); });
    }
    if (layers_panel_) {
        layers_panel_->set_rooms_list_container(rooms_list_container_.get());
        layers_panel_->set_layer_controls_container(layer_controls_container_.get());
        layers_panel_->set_on_layer_selected([this](int index) {
            if (layer_controls_display_) {
                layer_controls_display_->set_selected_layer(index);
            }
        });
    }

    ensure_room_configurator();

    if (!layers_preview_panel_) {
        layers_preview_panel_ = std::make_unique<MapLayersPreviewPanel>(kDefaultPanelX + 352, kDefaultPanelY + 48);
        layers_preview_panel_->close();
        track_floating_panel(layers_preview_panel_.get());
        layers_preview_panel_->set_on_select_layer([this](int layer_index) {
            this->set_active_panel(PanelType::Layers);
            if (layers_panel_) {
                layers_panel_->force_layer_controls_on_next_select();
                layers_panel_->select_layer(layer_index);
            }
        });
        layers_preview_panel_->set_on_select_room([this](const std::string& room_key) {
            this->set_active_panel(PanelType::Layers);
            if (layers_panel_) {
                layers_panel_->select_room(room_key);
            }
        });
        layers_preview_panel_->set_on_show_room_list([this]() {
            this->set_active_panel(PanelType::Layers);
            if (layers_panel_) {
                layers_panel_->show_room_list();
            }
        });
    }
    if (layers_preview_panel_ && layers_controller_) {
        layers_preview_panel_->set_controller(layers_controller_);
    }
    if (layers_preview_panel_ && map_info_) {
        layers_preview_panel_->set_map_info(map_info_, [this]() { return auto_save_layers_data(); });
    }
    if (!footer_bar_) {
        footer_bar_ = std::make_unique<DevFooterBar>("");
        footer_bar_->set_bounds(screen_w_, screen_h_);
        footer_bar_->set_title_visible(false);
        footer_bar_->set_visible(footer_always_visible_ || map_mode_active_);
        footer_buttons_configured_ = false;
    }
    if (footer_bar_ && !footer_buttons_configured_) {
        configure_footer_buttons();
        sync_footer_button_states();
    }
    update_footer_visibility();
    rebuild_floating_stack();
}

void MapModeUI::configure_footer_buttons() {
    if (!footer_bar_) return;

    std::vector<DevFooterBar::Button> buttons;

    auto append_custom = [&](std::vector<HeaderButtonConfig>& configs, HeaderMode mode) {
        auto append_button = [&](HeaderButtonConfig& config) {
            DevFooterBar::Button extra;
            extra.id = config.id;
            extra.label = config.label;
            extra.active = config.active;
            extra.momentary = config.momentary;
            extra.group = config.group;
            extra.style_override = config.style_override;
            extra.active_style_override = config.active_style_override;
            auto* cfg_ptr = &config;
            extra.on_toggle = [this, cfg_ptr, mode](bool active) {
                if (cfg_ptr->on_toggle) {
                    cfg_ptr->on_toggle(active);
                }
                if (cfg_ptr->momentary) {
                    set_button_state(mode, cfg_ptr->id, false);
                } else {
                    set_button_state(mode, cfg_ptr->id, active);
                }
};
            buttons.push_back(std::move(extra));
};

        for (auto& config : configs) {
            append_button(config);
        }
};

    {
        const bool has_layers_button = std::any_of(room_mode_buttons_.begin(), room_mode_buttons_.end(),
                                                  [](const HeaderButtonConfig& cfg) {
                                                      return cfg.id == "layers";
                                                  });

        if (!has_layers_button) {
            DevFooterBar::Button layers_btn;
            layers_btn.id = "layers";
            layers_btn.label = "Layers";
            layers_btn.group = FooterButtonGroup::Primary;
            layers_btn.style_override = &DMStyles::HeaderButton();
            layers_btn.active_style_override = &DMStyles::AccentButton();
            layers_btn.on_toggle = [this](bool active) {
                if (active) {
                    this->set_active_panel(PanelType::Layers);
                } else {
                    this->set_active_panel(PanelType::None);
                }
};
            buttons.push_back(std::move(layers_btn));
        }

        DevFooterBar::Button save_btn;
        save_btn.id = "save";
        save_btn.label = "Save";
        save_btn.group = FooterButtonGroup::Actions;
        save_btn.momentary = true;
        save_btn.style_override = &DMStyles::SecondaryButton();
        save_btn.active_style_override = &DMStyles::AccentButton();
        save_btn.on_toggle = [this](bool) {
            this->save_all_now(devmode::core::DevSaveCoordinator::Priority::Immediate);
        };
        buttons.push_back(std::move(save_btn));

        append_custom(room_mode_buttons_, HeaderMode::Room);
    }

    footer_bar_->set_buttons(std::move(buttons));
    footer_buttons_configured_ = true;
    sync_footer_button_states();
}

void MapModeUI::sync_footer_button_states() {
    if (!footer_bar_) return;
    const bool layers_visible = layers_panel_ && layers_panel_->is_visible();
    footer_bar_->set_button_active_state("layers", layers_visible);
    for (const auto& config : room_mode_buttons_) {
        footer_bar_->set_button_active_state(config.id, config.active);
    }
}

void MapModeUI::update_footer_visibility() {
    if (!footer_bar_) return;
    footer_bar_->set_bounds(screen_w_, screen_h_);

    const bool should_show = (!headers_suppressed_) && (footer_always_visible_ || map_mode_active_);
    footer_bar_->set_visible(should_show);
}

void MapModeUI::set_active_panel(PanelType panel) {
    ensure_panels();

    if (panel == PanelType::Layers && !ensure_panel_unlocked(layers_panel_.get(), "Layers")) {
        sync_footer_button_states();
        return;
    }

    PanelType new_active = PanelType::None;

    if (panel == PanelType::Layers) {
        if (layers_panel_) {
            ensure_room_configurator();
            layers_panel_->open();
            bring_panel_to_front(layers_panel_.get());
            layers_panel_->hide_details_panel();
        }
        show_sliding_panel(SlidingPanel::RoomsList);
        new_active = PanelType::Layers;
    } else {
        if (layers_panel_) {
            layers_panel_->hide_details_panel();
        }
        show_sliding_panel(SlidingPanel::None);
        close_room_configuration(false);
    }

    active_panel_ = new_active;
    sync_footer_button_states();
}

void MapModeUI::sync_panel_map_info() {
    if (!map_info_) return;
    ensure_panels();
    if (layers_panel_) {
        if (layers_controller_) {
            layers_controller_->set_manifest_store(manifest_store_, map_id_);
            layers_controller_->set_save_manager(save_manager_);
            layers_controller_->bind(map_info_, map_path_);
        }
        layers_panel_->set_map_info(map_info_, map_path_);
        layers_panel_->set_on_save([this]() { return auto_save_layers_data(); });
    }
    if (rooms_display_) {
        rooms_display_->set_map_info(map_info_);
    }
    if (layer_controls_display_) {
        layer_controls_display_->set_controller(layers_controller_);
        layer_controls_display_->set_selected_layer(layers_panel_ ? layers_panel_->selected_layer() : -1);
        layer_controls_display_->refresh();
    }
}

void MapModeUI::update(const Input& input) {
    ensure_panels();
    if (map_color_sampling_active_) {
        map_color_sampling_cursor_.x = input.getX();
        map_color_sampling_cursor_.y = input.getY();
    }
    if (footer_bar_ && footer_bar_->visible()) {
        footer_bar_->update(input);
    }
    if (layers_panel_ && layers_panel_->is_visible()) {
        layers_panel_->update(input, screen_w_, screen_h_);
    }
    for (DockableCollapsible* panel : floating_panels_) {
        if (!panel) continue;
        if (auto* layers_preview = dynamic_cast<MapLayersPreviewPanel*>(panel)) {
            if (layers_preview->is_visible()) {
                layers_preview->update(input, screen_w_, screen_h_);
            }
            continue;
        }
        if (panel->is_visible()) {
            panel->update(input, screen_w_, screen_h_);
        }
    }

    PanelType visible = PanelType::None;
    if (layers_panel_ && layers_panel_->is_visible()) {
        visible = PanelType::Layers;
    }
    if (visible != active_panel_) {
        active_panel_ = visible;
        sync_footer_button_states();
    }

    if (room_configurator_ && room_configurator_->visible()) {
        room_configurator_->update(input, screen_w_, screen_h_);
    }
    if (room_config_container_ && room_config_container_->is_visible()) {
        room_config_container_->update(input, screen_w_, screen_h_);
    }
    if (rooms_list_container_ && rooms_list_container_->is_visible()) {
        rooms_list_container_->update(input, screen_w_, screen_h_);
    }
    if (layer_controls_container_ && layer_controls_container_->is_visible()) {
        layer_controls_container_->update(input, screen_w_, screen_h_);
    }
}

bool MapModeUI::handle_event(const SDL_Event& e) {
    ensure_panels();
    if (map_color_sampling_active_) {
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            map_color_sampling_cursor_ = event_point(e);
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            SDL_Color chosen = map_color_sampling_preview_valid_ ? map_color_sampling_preview_ : SDL_Color{0, 0, 0, 255};
            complete_map_color_sampling(chosen);
            return true;
        }
        if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT) ||
            (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
            cancel_map_color_sampling();
            return true;
        }
        switch (e.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                return true;
            default:
                break;
        }
    }
    if (room_config_container_ && room_config_container_->is_visible() && room_config_container_->handle_event(e)) {
        return true;
    }
    if (rooms_list_container_ && rooms_list_container_->is_visible() && rooms_list_container_->handle_event(e)) {
        return true;
    }
    if (layer_controls_container_ && layer_controls_container_->is_visible() && layer_controls_container_->handle_event(e)) {
        return true;
    }
    if (layers_panel_ && layers_panel_->is_visible() && layers_panel_->handle_event(e)) {
        return true;
    }
    bool floating_used = false;
    if (handle_floating_panel_event(e, floating_used)) {
        return true;
    }
    if (floating_used) {
        return true;
    }

    bool footer_used = false;
    if (footer_bar_ && footer_bar_->visible()) {
        footer_used = footer_bar_->handle_event(e);
    }
    if (footer_used) {
        return true;
    }

    return false;
}

void MapModeUI::render(SDL_Renderer* renderer) const {
    if (layers_panel_ && layers_panel_->is_visible()) {
        layers_panel_->render(renderer);
    }
    for (DockableCollapsible* panel : floating_panels_) {
        if (!panel) continue;
        if (auto* layers_preview = dynamic_cast<MapLayersPreviewPanel*>(panel)) {
            if (layers_preview->is_visible()) {
                layers_preview->render(renderer);
            }
            continue;
        }
        if (panel->is_visible()) {
            panel->render(renderer);
        }
    }
    if (room_config_container_ && room_config_container_->is_visible()) {
        room_config_container_->render(renderer, screen_w_, screen_h_);
    }
    if (rooms_list_container_ && rooms_list_container_->is_visible()) {
        rooms_list_container_->render(renderer, screen_w_, screen_h_);
    }
    if (layer_controls_container_ && layer_controls_container_->is_visible()) {
        layer_controls_container_->render(renderer, screen_w_, screen_h_);
    }
    if (footer_bar_ && footer_bar_->visible()) {
        footer_bar_->render(renderer);
    }
    if (map_color_sampling_active_ && renderer) {
        SDL_Rect sample_rect{map_color_sampling_cursor_.x, map_color_sampling_cursor_.y, 1, 1};
        Uint32 pixel = 0;
        if (SDL_Surface* captured = SDL_RenderReadPixels(renderer, &sample_rect)) {
            SDL_Surface* working = captured;
            if (captured->format != SDL_PIXELFORMAT_ARGB8888) {
                working = SDL_ConvertSurface(captured, SDL_PIXELFORMAT_ARGB8888);
                SDL_DestroySurface(captured);
                captured = nullptr;
            }
            if (working && working->pixels) {
                pixel = *static_cast<const Uint32*>(working->pixels);
                SDL_DestroySurface(working);
            } else {
                if (working) SDL_DestroySurface(working);
                map_color_sampling_preview_valid_ = false;
                return;
            }
            Uint8 r = 0, g = 0, b = 0, a = 0;
            if (const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_ARGB8888)) {
                SDL_GetRGBA(pixel, format, nullptr, &r, &g, &b, &a);
                map_color_sampling_preview_ = SDL_Color{r, g, b, a};
                map_color_sampling_preview_valid_ = true;
            } else {
                map_color_sampling_preview_valid_ = false;
            }
        } else {
            map_color_sampling_preview_valid_ = false;
        }
        const int preview_size = 48;
        SDL_Rect preview_rect{map_color_sampling_cursor_.x + 18,
                              map_color_sampling_cursor_.y + 18,
                              preview_size,
                              preview_size};
        SDL_Rect inner_rect{preview_rect.x + 4,
                            preview_rect.y + 4,
                            std::max(0, preview_rect.w - 8), std::max(0, preview_rect.h - 8)};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
        sdl_render::FillRect(renderer, &preview_rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
        sdl_render::Rect(renderer, &preview_rect);
        if (map_color_sampling_preview_valid_) {
            SDL_SetRenderDrawColor(renderer, map_color_sampling_preview_.r, map_color_sampling_preview_.g, map_color_sampling_preview_.b, 255);
            sdl_render::FillRect(renderer, &inner_rect);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
            sdl_render::Rect(renderer, &inner_rect);
        } else {
            SDL_SetRenderDrawColor(renderer, 120, 120, 120, 220);
            sdl_render::Rect(renderer, &inner_rect);
        }
    }
}

void MapModeUI::open_layers_panel() {
    ensure_panels();
    if (!ensure_panel_unlocked(layers_panel_.get(), "Layers")) {
        return;
    }
    set_active_panel(PanelType::Layers);
}

void MapModeUI::toggle_layers_panel() {
    ensure_panels();
    if (!ensure_panel_unlocked(layers_panel_.get(), "Layers")) {
        sync_footer_button_states();
        return;
    }
    if (active_panel_ == PanelType::Layers) {
        set_active_panel(PanelType::None);
    } else {
        set_active_panel(PanelType::Layers);
    }
}

void MapModeUI::close_all_panels() {
    if (layers_preview_panel_) {
        layers_preview_panel_->close();
    }
    set_active_panel(PanelType::None);
    close_room_configuration(false);
}

SDL_Rect MapModeUI::room_config_bounds() const {
    if (screen_w_ <= 0 || screen_h_ <= 0) {
        return SDL_Rect{0, 0, 0, 0};
    }
    SDL_Rect area = sanitize_sliding_area(sliding_area_bounds_);
    if (area.w <= 0 || area.h <= 0) {
        area = SDL_Rect{0, 0, screen_w_, screen_h_};
    }
    int area_x = area.x;
    int area_y = area.y;
    int area_w = area.w;
    int area_h = area.h;

    int panel_x = area_x + (area_w * 2) / 3;
    int panel_w = area_w - (panel_x - area_x);
    const int min_width = std::max(320, screen_w_ / 3);
    if (panel_w < min_width) {
        panel_w = std::min(min_width, area_w);
        panel_x = area_x + std::max(0, area_w - panel_w);
    }
    if (panel_w > area_w) {
        panel_w = area_w;
        panel_x = area_x;
    }
    panel_x = std::clamp(panel_x, area_x, area_x + std::max(0, area_w - panel_w));
    return SDL_Rect{panel_x, area_y, std::max(0, panel_w), std::max(0, area_h)};
}

void MapModeUI::show_sliding_panel(SlidingPanel panel, bool) {
    if (room_config_container_) {
        room_config_container_->set_visible(false);
    }
    if (rooms_list_container_) {
        rooms_list_container_->set_visible(false);
    }
    if (layer_controls_container_) {
        layer_controls_container_->set_visible(false);
    }

    switch (panel) {
        case SlidingPanel::RoomConfig:
            if (room_config_container_) {
                room_config_container_->open();
            }
            active_sliding_panel_ = SlidingPanel::RoomConfig;
            break;
        case SlidingPanel::RoomsList:
            if (rooms_list_container_) {
                rooms_list_container_->open();
            }
            active_sliding_panel_ = SlidingPanel::RoomsList;
            break;
        case SlidingPanel::LayerControls:
            if (layer_controls_container_) {
                layer_controls_container_->open();
            }
            active_sliding_panel_ = SlidingPanel::LayerControls;
            break;
        case SlidingPanel::None:
        default:
            active_sliding_panel_ = SlidingPanel::None;
            break;
    }
}

void MapModeUI::ensure_room_configurator() {
    if (!room_configurator_) {
        room_configurator_ = std::make_unique<RoomConfigurator>();
    }
    if (room_configurator_) {

        room_configurator_->set_header_visibility_controller([this](bool visible) {
            this->set_dev_sliding_headers_hidden(visible);
        });
        room_configurator_->set_on_close([this]() {
            active_room_config_key_.clear();
            if (rooms_display_) {
                rooms_display_->refresh();
            }
            this->show_sliding_panel(room_config_return_panel_);
        });
        room_configurator_->set_blocks_editor_interactions(false);
        room_configurator_->set_on_camera_changed([this](Room* room) {
            if (assets_) {
                if (!room || room == assets_->current_room()) {
                    assets_->getView().set_manual_height_override(false);
                    assets_->getView().set_manual_zoom_override(false);
                    assets_->mark_camera_dirty();
                }
            }
        });
        room_configurator_->set_spawn_group_callbacks(
            {},
            [this](const std::string& spawn_id) { this->delete_active_room_spawn_group(spawn_id); },
            [this](const std::string& spawn_id, size_t index) {
                this->reorder_active_room_spawn_group(spawn_id, index);
            },
            {},
            {});
        room_configurator_->set_on_room_renamed([this](const std::string& old_name, const std::string& desired) {
            return this->rename_active_room(old_name, desired);
        });
    }
    if (!room_config_container_) {
        room_config_container_ = std::make_unique<SlidingWindowContainer>();
        if (room_config_container_) {
            room_config_container_->set_header_visible(true);
            room_config_container_->set_scrollbar_visible(true);

            room_config_container_->set_header_visibility_controller([this](bool visible) {
                this->set_dev_sliding_headers_hidden(visible);
            });
            room_config_container_->set_blocks_editor_interactions(false);
        }
    }
    if (room_config_container_) {
        room_config_container_->set_close_button_enabled(true);
    }
    if (room_configurator_ && room_config_container_) {
        room_configurator_->attach_container(room_config_container_.get());
        apply_sliding_area_bounds();
    }
    update_room_config_header_controls();
}

void MapModeUI::open_room_configuration(const std::string& room_key, SlidingPanel return_panel) {
    ensure_panels();
    ensure_room_configurator();
    if (!room_configurator_ || !map_info_) {
        return;
    }

    room_config_return_panel_ = return_panel;
    update_room_config_header_controls();

    nlohmann::json& map_info = *map_info_;
    nlohmann::json& rooms = map_info["rooms_data"];
    if (!rooms.is_object()) {
        rooms = nlohmann::json::object();
    }
    nlohmann::json& room_entry = rooms[room_key];
    if (!room_entry.is_object()) {
        room_entry = nlohmann::json::object();
        room_entry["name"] = room_key;
    }

    active_room_config_key_ = room_key;
    if (layers_panel_) {
        layers_panel_->hide_details_panel();
    }

    auto on_change = [this]() {
        mark_map_data_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        if (layers_panel_) {
            layers_panel_->mark_dirty(true);
        }
        if (rooms_display_) {
            rooms_display_->refresh();
        }
};
    auto on_entry_change = [this](const nlohmann::json&, const auto&) {
        mark_map_data_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        if (layers_panel_) {
            layers_panel_->mark_dirty(true);
        }
};

    apply_sliding_area_bounds();
    room_configurator_->open(room_entry, on_change, on_entry_change, {});
    show_sliding_panel(SlidingPanel::RoomConfig);
}

void MapModeUI::close_room_configuration(bool show_rooms_list) {
    if (room_configurator_) {
        room_configurator_->close();
    }
    active_room_config_key_.clear();
    if (show_rooms_list) {
        room_config_return_panel_ = SlidingPanel::RoomsList;
        show_sliding_panel(room_config_return_panel_);
    } else {
        room_config_return_panel_ = SlidingPanel::None;
        show_sliding_panel(room_config_return_panel_);
    }
    update_room_config_header_controls();
}

bool MapModeUI::is_point_inside(int x, int y) const {
    if (pointer_inside_floating_panel(x, y)) {
        return true;
    }
    if (headers_suppressed_ && !sliding_only_header_suppression_) {
        return false;
    }
    if (footer_bar_ && footer_bar_->visible() && footer_bar_->contains(x, y)) {
        return true;
    }
    if (layers_panel_ && layers_panel_->is_visible() && layers_panel_->is_point_inside(x, y)) {
        return true;
    }
    if (room_config_container_ && room_config_container_->is_visible() && room_config_container_->is_point_inside(x, y)) {
        return true;
    }
    if (rooms_list_container_ && rooms_list_container_->is_visible() && rooms_list_container_->is_point_inside(x, y)) {
        return true;
    }
    if (layer_controls_container_ && layer_controls_container_->is_visible() && layer_controls_container_->is_point_inside(x, y)) {
        return true;
    }
    return false;
}

bool MapModeUI::is_any_panel_visible() const {
    for (DockableCollapsible* panel : floating_panels_) {
        if (!panel) continue;
        if (auto* layers_preview = dynamic_cast<MapLayersPreviewPanel*>(panel)) {
            if (layers_preview->is_visible()) return true;
            continue;
        }
        if (panel->is_visible()) return true;
    }
    if (room_config_container_ && room_config_container_->is_visible()) return true;
    if (rooms_list_container_ && rooms_list_container_->is_visible()) return true;
    if (layer_controls_container_ && layer_controls_container_->is_visible()) return true;
    return layers_panel_ && layers_panel_->is_visible();
}

bool MapModeUI::is_layers_panel_visible() const {
    return layers_panel_ && layers_panel_->is_visible();
}

bool MapModeUI::save_map_info_to_disk(devmode::core::DevSaveCoordinator::Priority priority) const {
    if (!map_info_) return false;
    if (dirty_callback_) {
        dirty_callback_(priority);
        return true;
    }
    return false;
}

bool MapModeUI::save_all_now(devmode::core::DevSaveCoordinator::Priority priority) const {
    if (!save_manager_) {
        std::cerr << "[MapModeUI] Save requested but SaveManager is unavailable\n";
        return false;
    }
    const bool saved = save_manager_->save_dirty(priority, "Dev footer save all");
    if (!saved) {
        std::cerr << "[MapModeUI] Save request completed with no dirty entries\n";
    }
    return saved;
}

bool MapModeUI::mutate_map_data(const std::function<bool(manifest::MapData&)>& mutator) {
    if (!assets_) {
        return false;
    }
    return assets_->mutate_map_data(mutator);
}

void MapModeUI::mark_map_data_dirty(devmode::core::DevSaveCoordinator::Priority priority) {
    if (assets_) {
        assets_->mark_map_data_dirty();
    }
    if (dirty_callback_) {
        dirty_callback_(priority);
    }
}

bool MapModeUI::auto_save_layers_data() {
    bool saved = false;
    if (assets_) {
        assets_->mark_map_data_dirty();
    }
    if (dirty_callback_) {
        dirty_callback_(devmode::core::DevSaveCoordinator::Priority::Debounced);
        saved = true;
    }
    if (rooms_display_) {
        rooms_display_->refresh();
    }
    if (layer_controls_display_) {
        layer_controls_display_->refresh();
    }
    if (layers_panel_) {
        layers_panel_->mark_dirty(true);
    }
    return saved;
}

nlohmann::json* MapModeUI::active_room_entry() {
    if (!map_info_ || active_room_config_key_.empty()) {
        return nullptr;
    }
    nlohmann::json& map_info = *map_info_;
    nlohmann::json& rooms = map_info["rooms_data"];
    if (!rooms.is_object()) {
        return nullptr;
    }
    auto it = rooms.find(active_room_config_key_);
    if (it == rooms.end() || !it->is_object()) {
        return nullptr;
    }
    return &it.value();
}

std::string MapModeUI::rename_active_room(const std::string& old_name, const std::string& desired_name) {
    std::string trimmed = trim_copy(desired_name);
    std::string base = sanitize_room_key(trimmed.empty() ? desired_name : trimmed);
    if (!map_info_) {
        return base.empty() ? old_name : base;
    }

    std::string result_key = old_name;
    mutate_map_data([&](manifest::MapData& map_data) {
        nlohmann::json map_info = map_data.to_manifest_entry();
        nlohmann::json& rooms = map_info["rooms_data"];
        if (!rooms.is_object()) {
            rooms = nlohmann::json::object();
        }

        std::string current_key = active_room_config_key_.empty() ? old_name : active_room_config_key_;
        if (!rooms.contains(current_key)) {
            current_key = old_name;
        }
        if (!rooms.contains(current_key)) {
            result_key = base.empty() ? old_name : base;
            return false;
        }

        std::string candidate = base.empty() ? current_key : base;
        if (candidate.empty()) {
            candidate = current_key;
        }

        nlohmann::json entry = rooms[current_key];
        entry["name"] = desired_name;

        if (candidate == current_key || rooms.contains(candidate)) {
            rooms[current_key] = std::move(entry);
            result_key = current_key;
        } else {
            rooms.erase(current_key);
            rooms[candidate] = std::move(entry);
            map_layers::rename_room_references_in_layers(map_info, current_key, candidate);
            result_key = candidate;
        }

        map_data = manifest::MapData::from_manifest_entry(map_data.map_id, map_info);
        return true;
    });

    const bool renaming_active = !active_room_config_key_.empty();
    if (renaming_active) {
        active_room_config_key_ = result_key;
    }
    handle_rooms_data_mutated(true);
    if (renaming_active && room_configurator_ && active_room_config_key_ == result_key) {
        if (nlohmann::json* entry = active_room_entry()) {
            room_configurator_->refresh_spawn_groups(*entry);
        }
    }
    return result_key;
}


void MapModeUI::update_room_config_header_controls() {
    if (!room_config_container_) {
        return;
    }
    room_config_container_->set_close_button_enabled(true);
    room_config_container_->clear_header_navigation_button();
}

void MapModeUI::delete_active_room_spawn_group(const std::string& spawn_id) {
    if (spawn_id.empty()) {
        return;
    }
    if (active_room_config_key_.empty()) {
        return;
    }

    bool changed = false;
    mutate_map_data([&](manifest::MapData& map_data) {
        nlohmann::json map_entry = map_data.to_manifest_entry();
        nlohmann::json& rooms = map_entry["rooms_data"];
        if (!rooms.is_object()) {
            return false;
        }
        auto it_room = rooms.find(active_room_config_key_);
        if (it_room == rooms.end() || !it_room->is_object()) {
            return false;
        }

        nlohmann::json& groups = ensure_spawn_groups_array(it_room.value());
        auto it = std::remove_if(groups.begin(), groups.end(), [&](nlohmann::json& entry) {
            if (!entry.is_object()) return false;
            if (!entry.contains("spawn_id") || !entry["spawn_id"].is_string()) return false;
            return entry["spawn_id"].get<std::string>() == spawn_id;
        });
        if (it == groups.end()) {
            return false;
        }
        groups.erase(it, groups.end());
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].is_object()) {
                groups[i]["priority"] = static_cast<int>(i);
            }
        }
        sanitize_perimeter_spawn_groups(groups);
        map_data = manifest::MapData::from_manifest_entry(map_data.map_id, map_entry);
        changed = true;
        return true;
    });

    if (!changed) {
        return;
    }
    if (room_configurator_) {
        if (nlohmann::json* room_entry = active_room_entry()) {
            room_configurator_->refresh_spawn_groups(*room_entry);
        }
        room_configurator_->notify_spawn_groups_mutated();
    }
    handle_rooms_data_mutated(true);
}

void MapModeUI::reorder_active_room_spawn_group(const std::string& spawn_id, size_t index) {
    if (spawn_id.empty()) {
        return;
    }
    if (active_room_config_key_.empty()) {
        return;
    }

    bool changed = false;
    mutate_map_data([&](manifest::MapData& map_data) {
        nlohmann::json map_entry = map_data.to_manifest_entry();
        nlohmann::json& rooms = map_entry["rooms_data"];
        if (!rooms.is_object()) {
            return false;
        }
        auto it_room = rooms.find(active_room_config_key_);
        if (it_room == rooms.end() || !it_room->is_object()) {
            return false;
        }

        nlohmann::json& groups = ensure_spawn_groups_array(it_room.value());
        if (!groups.is_array() || groups.empty()) {
            return false;
        }

        auto it = std::find_if(groups.begin(), groups.end(), [&](const nlohmann::json& entry) {
            if (!entry.is_object()) return false;
            if (!entry.contains("spawn_id") || !entry["spawn_id"].is_string()) return false;
            return entry["spawn_id"].get<std::string>() == spawn_id;
        });
        if (it == groups.end()) {
            return false;
        }

        nlohmann::json moved = *it;
        groups.erase(it);
        size_t clamped = std::min(index, groups.size());
        groups.insert(groups.begin() + static_cast<std::ptrdiff_t>(clamped), std::move(moved));

        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].is_object()) {
                groups[i]["priority"] = static_cast<int>(i);
            }
        }

        map_data = manifest::MapData::from_manifest_entry(map_data.map_id, map_entry);
        changed = true;
        return true;
    });

    if (!changed) {
        return;
    }
    if (room_configurator_) {
        if (nlohmann::json* room_entry = active_room_entry()) {
            room_configurator_->refresh_spawn_groups(*room_entry);
        }
        room_configurator_->notify_spawn_groups_mutated();
    }
    handle_rooms_data_mutated(false);
}

void MapModeUI::handle_rooms_data_mutated(bool refresh_rooms_list) {
    if (!map_info_) {
        return;
    }
    if (assets_) {
        assets_->mark_map_data_dirty();
    }
    if (layers_panel_) {
        layers_panel_->mark_dirty(true);
    }
    if (refresh_rooms_list && rooms_display_) {
        rooms_display_->refresh();
    }
    if (layer_controls_display_) {
        layer_controls_display_->refresh();
    }
}

void MapModeUI::create_room_from_layers_controls() {
    create_room_from_panel(SlidingPanel::LayerControls);
}

void MapModeUI::create_room_from_panel(SlidingPanel return_panel) {
    if (!map_info_ || !map_info_->is_object()) {
        return;
    }
    std::string new_key;
    mutate_map_data([&](manifest::MapData& map_data) {
        nlohmann::json map_entry = map_data.to_manifest_entry();
        new_key = map_layers::create_room_entry(map_entry);
        if (new_key.empty()) {
            return false;
        }
        map_data = manifest::MapData::from_manifest_entry(map_data.map_id, map_entry);
        return true;
    });
    if (new_key.empty()) {
        return;
    }
    handle_rooms_data_mutated(true);
    open_room_configuration(new_key, return_panel);
    auto_save_layers_data();
}

void MapModeUI::begin_map_color_sampling(const utils::color::RangedColor&,
                                         std::function<void(SDL_Color)> on_sample,
                                         std::function<void()> on_cancel) {
    if (!on_sample) {
        if (on_cancel) {
            on_cancel();
        }
        return;
    }
    cancel_map_color_sampling(true);
    map_color_sampling_active_ = true;
    map_color_sampling_preview_valid_ = false;
    map_color_sampling_apply_ = std::move(on_sample);
    map_color_sampling_cancel_ = std::move(on_cancel);
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    map_color_sampling_cursor_.x = mx;
    map_color_sampling_cursor_.y = my;
    if (!map_color_sampling_cursor_handle_) {
        map_color_sampling_cursor_handle_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    }
    map_color_sampling_prev_cursor_ = SDL_GetCursor();
    if (map_color_sampling_cursor_handle_) {
        SDL_SetCursor(map_color_sampling_cursor_handle_);
    }
}

void MapModeUI::cancel_map_color_sampling(bool silent) {
    if (!map_color_sampling_active_) {
        return;
    }
    map_color_sampling_active_ = false;
    map_color_sampling_preview_valid_ = false;
    if (map_color_sampling_prev_cursor_) {
        SDL_SetCursor(map_color_sampling_prev_cursor_);
        map_color_sampling_prev_cursor_ = nullptr;
    }
    auto cancel_cb = std::move(map_color_sampling_cancel_);
    map_color_sampling_apply_ = nullptr;
    map_color_sampling_cancel_ = nullptr;
    if (!silent && cancel_cb) {
        cancel_cb();
    }
}

void MapModeUI::complete_map_color_sampling(SDL_Color color) {
    auto apply_cb = std::move(map_color_sampling_apply_);
    cancel_map_color_sampling(true);
    if (apply_cb) {
        apply_cb(color);
    }
}


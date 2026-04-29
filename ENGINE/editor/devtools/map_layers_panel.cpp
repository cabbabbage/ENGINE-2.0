#include "map_layers_panel.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <SDL3_ttf/SDL_ttf.h>

#include "draw_utils.hpp"
#include "dm_styles.hpp"
#include "font_cache.hpp"
#include "map_layers_container_configurator.hpp"
#include "map_layers_controller.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "utils/display_color.hpp"
#include "utils/input.hpp"
#include "utils/ranged_color.hpp"
#include "utils/ttf_render_utils.hpp"
#include "dev_mode_color_utils.hpp"
#include "dev_mode_sdl_event_utils.hpp"

namespace {

constexpr int kMinimumListHeight = 200;
constexpr int kRowHeight = 52;
constexpr int kDropIndicatorThickness = 3;
constexpr int kLayerDeleteButtonSize = 26;

const DMLabelStyle& summary_label_style() {
    static DMLabelStyle style{DMStyles::Label().font_path, std::max(12, DMStyles::Label().font_size - 2),
                              SDL_Color{189, 200, 214, 255}};
    return style;
}

const DMLabelStyle& validation_label_style() {
    static DMLabelStyle style{DMStyles::Label().font_path, std::max(12, DMStyles::Label().font_size - 3),
                              SDL_Color{200, 210, 225, 255}};
    return style;
}

SDL_Color error_color() { return SDL_Color{220, 53, 69, 255}; }
SDL_Color warning_color() { return SDL_Color{234, 179, 8, 255}; }
SDL_Color success_color() { return SDL_Color{16, 185, 129, 255}; }
SDL_Color info_color() { return SDL_Color{148, 163, 184, 255}; }

std::string trimmed(std::string value) {
    auto begin = std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); });
    value.erase(value.begin(), begin);
    auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); });
    value.erase(end.base(), value.end());
    return value;
}

SDL_Color severity_color(bool has_error, bool has_warning, bool highlighted) {
    if (has_error) {
        SDL_Color c = error_color();
        return highlighted ? lighten(c, 0.25f) : c;
    }
    if (has_warning) {
        SDL_Color c = warning_color();
        return highlighted ? lighten(c, 0.25f) : c;
    }
    SDL_Color neutral = DMStyles::Border();
    return highlighted ? lighten(neutral, 0.35f) : neutral;
}

SDL_Color severity_fill(bool has_error, bool has_warning, bool selected) {
    if (has_error) {
        SDL_Color base{120, 40, 48, 240};
        return selected ? lighten(base, 0.2f) : base;
    }
    if (has_warning) {
        SDL_Color base{120, 92, 40, 235};
        return selected ? lighten(base, 0.2f) : base;
    }
    SDL_Color base = DMStyles::ButtonBaseFill();
    return selected ? lighten(base, 0.22f) : base;
}

}

class MapLayersPanel::LayersListWidget : public Widget {
public:
    explicit LayersListWidget(MapLayersPanel* owner) : owner_(owner) {}

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        if (owner_) {
            owner_->update_layer_row_geometry();
        }
    }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int w) const override {
        (void)w;
        if (!owner_) {
            return kMinimumListHeight;
        }
        return owner_->list_height_for_width(w);
    }

    bool handle_event(const SDL_Event& e) override {
        if (!owner_) {
            return false;
        }

        if (owner_->is_dragging_layer()) {
            switch (e.type) {
                case SDL_EVENT_MOUSE_MOTION:
                    owner_->on_layers_list_mouse_motion(e.motion.y, static_cast<Uint32>(e.motion.state));
                    return true;
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    SDL_Point p = event_point_from_event(e);
                    owner_->on_layers_list_mouse_up(p.y, e.button.button);
                    return true;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.button == SDL_BUTTON_RIGHT) {
                        owner_->cancel_drag();
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }

        switch (e.type) {
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                SDL_Point p = event_point_from_event(e);
                if (!SDL_PointInRect(&p, &rect_)) {
                    if (e.type == SDL_EVENT_MOUSE_MOTION) {
                        owner_->clear_hover();
                    }
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && owner_->is_dragging_layer()) {
                        owner_->cancel_drag();
                    }
                    return false;
                }

                int hit_index = -1;
                int delete_hit_index = -1;
                for (const auto& row : owner_->layer_rows_) {
                    if (SDL_PointInRect(&p, &row.rect)) {
                        hit_index = row.index;
                        if (SDL_PointInRect(&p, &row.delete_button_rect)) {
                            delete_hit_index = row.index;
                        }
                        break;
                    }
                }

                owner_->set_hovered_delete_layer(delete_hit_index);

                if (e.type == SDL_EVENT_MOUSE_MOTION) {
                    if (hit_index >= 0) {
                        owner_->set_hovered_layer(hit_index);
                    } else {
                        owner_->clear_hover();
                    }
                    return false;
                }

                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    if (hit_index >= 0) {
                        owner_->set_hovered_layer(hit_index);
                        if (delete_hit_index >= 0) {
                            owner_->on_delete_layer_clicked(delete_hit_index);
                            return true;
                        }
                        owner_->on_layers_list_mouse_down(hit_index, p.y);
                        return true;
                    }
                    return false;
                }

                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                    if (delete_hit_index >= 0) {
                        owner_->set_hovered_delete_layer(-1);
                        return true;
                    }
                    owner_->on_layers_list_mouse_up(p.y, e.button.button);
                    return true;
                }
                return false;
            }
            default:
                break;
        }
        return false;
    }

    void render(SDL_Renderer* renderer) const override {
        if (owner_) {
            owner_->render_layers_list(renderer);
        }
    }

    bool wants_full_row() const override { return true; }

private:
    MapLayersPanel* owner_ = nullptr;
    SDL_Rect rect_{0, 0, 0, 0};
};

class MapLayersPanel::ValidationSummaryWidget : public Widget {
public:
    explicit ValidationSummaryWidget(MapLayersPanel* owner) : owner_(owner) {}

    void set_rect(const SDL_Rect& r) override { rect_ = r; }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int w) const override {
        return owner_ ? owner_->validation_summary_height(w) : 0;
    }

    bool handle_event(const SDL_Event& e) override {
        (void)e;
        return false;
    }

    void render(SDL_Renderer* renderer) const override {
        if (owner_) {
            owner_->render_validation_summary(renderer, rect_);
        }
    }

    bool wants_full_row() const override { return true; }

private:
    MapLayersPanel* owner_ = nullptr;
    SDL_Rect rect_{0, 0, 0, 0};
};

class MapLayersPanel::MinEdgeWidget : public Widget {
public:
    explicit MinEdgeWidget(MapLayersPanel* owner) : owner_(owner) {}

    void mark_layout_dirty() { this->request_layout(); }

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        if (owner_) {
            owner_->layout_min_edge_input(rect_);
        }
    }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int w) const override {
        return owner_ ? owner_->min_edge_widget_height_for_width(w) : DMTextBox::height();
    }

    bool handle_event(const SDL_Event& e) override {
        if (!owner_) {
            return false;
        }
        return owner_->handle_min_edge_event(e);
    }

    void render(SDL_Renderer* renderer) const override {
        if (owner_) {
            owner_->render_min_edge_input(renderer, rect_);
        }
    }

    bool wants_full_row() const override { return true; }

private:
    MapLayersPanel* owner_ = nullptr;
    SDL_Rect rect_{0, 0, 0, 0};
};

MapLayersPanel::MapLayersPanel(int x, int y)
    : DockableCollapsible("Map Layers", true, x, y) {
    add_layer_button_ = std::make_unique<DMButton>("Add Layer", &DMStyles::CreateButton(), 140, DMButton::height());
    reload_button_ = std::make_unique<DMButton>("Reload", &DMStyles::WarnButton(), 120, DMButton::height());
    owned_widgets_.push_back(std::make_unique<ButtonWidget>(add_layer_button_.get(), [this]() {
        if (controller_) {
            const int created = controller_->create_layer();
            mark_dirty();
            if (created >= 0) {
                select_layer(created);
                trigger_save();
            }
        } else {
            nlohmann::json& layers = layers_array();
            const int new_index = static_cast<int>(layers.size());
            nlohmann::json layer = nlohmann::json::object();
            layer["name"] = std::string{"Layer "} + std::to_string(new_index);
            layers.push_back(std::move(layer));
            mark_dirty();
            select_layer(new_index);
            trigger_save();
        }
    }));
    Widget* add_widget = owned_widgets_.back().get();

    owned_widgets_.push_back(std::make_unique<ButtonWidget>(reload_button_.get(), [this]() {
        if (controller_ && controller_->reload()) {
            mark_dirty();
        }
        rebuild_layers();
    }));
    Widget* reload_widget = owned_widgets_.back().get();

    owned_widgets_.push_back(std::make_unique<LayersListWidget>(this));
    list_widget_ = static_cast<LayersListWidget*>(owned_widgets_.back().get());

    auto preview_widget_storage = std::make_unique<MapLayersPreviewWidget>();
    preview_widget_storage->set_on_select_layer([this](int index) {
        this->force_layer_controls_on_next_select();
        this->select_layer(index);
    });
    preview_widget_storage->set_on_select_room([this](const std::string& room_key) {
        this->select_room(room_key);
    });
    preview_widget_storage->set_on_show_room_list([this]() {
        this->show_room_list();
    });
    owned_widgets_.push_back(std::move(preview_widget_storage));
    preview_widget_ = static_cast<MapLayersPreviewWidget*>(owned_widgets_.back().get());
    preview_widget_->set_map_info(map_info_);
    preview_widget_->set_controller(controller_);
    preview_widget_->mark_dirty();

    min_edge_textbox_ = std::make_unique<DMTextBox>("Min room edge distance (px)", "");
    if (min_edge_textbox_) {
        min_edge_textbox_->set_on_height_changed([this]() {
            if (min_edge_widget_) {
                min_edge_widget_->mark_layout_dirty();
            }
        });
    }
    owned_widgets_.push_back(std::make_unique<MinEdgeWidget>(this));
    min_edge_widget_ = static_cast<MinEdgeWidget*>(owned_widgets_.back().get());

    owned_widgets_.push_back(std::make_unique<ValidationSummaryWidget>(this));
    validation_widget_ = static_cast<ValidationSummaryWidget*>(owned_widgets_.back().get());

    Rows rows;
    rows.push_back(Row{add_widget, reload_widget});
    rows.push_back(Row{list_widget_});
    rows.push_back(Row{preview_widget_});
    rows.push_back(Row{min_edge_widget_});
    rows.push_back(Row{validation_widget_});
    set_rows(rows);
    sync_min_edge_textbox();

    set_close_button_on_left(true);
    set_close_button_enabled(true);

    set_on_close([this]() {
        if (rooms_list_container_) {
            rooms_list_container_->close();
        }
        if (layer_controls_container_) {
            layer_controls_container_->close();
        }
    });
    set_expanded(true);
    set_visible(false);
}

MapLayersPanel::~MapLayersPanel() {
    remove_listener();
}

void MapLayersPanel::set_map_info(nlohmann::json* map_info, const std::string& map_path) {
    map_info_ = map_info;
    map_path_ = map_path;
    if (preview_widget_) {
        preview_widget_->set_map_info(map_info_);
        preview_widget_->mark_dirty();
    }
    sync_min_edge_textbox();
    mark_dirty();
}

void MapLayersPanel::set_on_save(SaveCallback cb) {
    on_save_ = std::move(cb);
}

void MapLayersPanel::set_controller(std::shared_ptr<MapLayersController> controller) {
    if (controller_ == controller) {
        return;
    }
    remove_listener();
    controller_ = std::move(controller);
    ensure_listener();
    if (preview_widget_) {
        preview_widget_->set_controller(controller_);
        preview_widget_->mark_dirty();
    }
    sync_min_edge_textbox();
    mark_dirty();
}

void MapLayersPanel::set_header_visibility_callback(std::function<void(bool)> cb) {
    header_visibility_callback_ = std::move(cb);
}

void MapLayersPanel::set_work_area(const SDL_Rect& bounds) {
    DockableCollapsible::set_work_area(bounds);
}

void MapLayersPanel::open() {
    set_visible(true);
    notify_header_visibility();
}

void MapLayersPanel::close() {
    set_visible(false);
    notify_header_visibility();
}

bool MapLayersPanel::is_visible() const {
    return DockableCollapsible::is_visible();
}

bool MapLayersPanel::room_config_visible() const {
    return false;
}

void MapLayersPanel::hide_main_container() {

}

void MapLayersPanel::show_room_list() {
    notify_side_panel(SidePanel::RoomsList);
}

void MapLayersPanel::select_room(const std::string& room_key) {
    pending_room_selection_ = room_key;
    if (on_configure_room_) {
        on_configure_room_(room_key);
    }
}

void MapLayersPanel::hide_details_panel() {
    notify_side_panel(SidePanel::RoomsList);
}

void MapLayersPanel::set_on_configure_room(std::function<void(const std::string&)> cb) {
    on_configure_room_ = std::move(cb);
}

void MapLayersPanel::set_on_layer_selected(std::function<void(int)> cb) {
    on_layer_selected_ = std::move(cb);
}

void MapLayersPanel::set_side_panel_callback(std::function<void(SidePanel)> cb) {
    side_panel_callback_ = std::move(cb);
}

void MapLayersPanel::force_layer_controls_on_next_select() {
    force_layer_controls_on_select_ = true;
}

void MapLayersPanel::set_rooms_list_container(SlidingWindowContainer* container) {
    if (rooms_list_container_ == container) {
        return;
    }
    rooms_list_container_ = container;
    if (rooms_list_container_) {
        map_layers::container_configurator::apply_default_panel_options(*rooms_list_container_, true);
    }
}

void MapLayersPanel::set_layer_controls_container(SlidingWindowContainer* container) {
    if (layer_controls_container_ == container) {
        return;
    }
    layer_controls_container_ = container;
    if (layer_controls_container_) {
        map_layers::container_configurator::apply_default_panel_options(*layer_controls_container_, true);
    }
}

void MapLayersPanel::set_embedded_mode(bool embedded) {
    if (embedded_mode_ == embedded) {
        return;
    }
    embedded_mode_ = embedded;
    set_floatable(!embedded_mode_);
    if (embedded_mode_) {
        if (embedded_bounds_.w > 0 && embedded_bounds_.h > 0) {
            set_rect(embedded_bounds_);
        }
        update_embedded_layout_constraints();
    } else {
        target_body_height_ = 0;
        set_available_height_override(-1);
        set_visible_height(default_visible_height_);
    }
}

void MapLayersPanel::set_embedded_bounds(const SDL_Rect& bounds) {
    embedded_bounds_ = bounds;
    if (embedded_mode_) {
        set_rect(bounds);
        update_embedded_layout_constraints();
    }
}

void MapLayersPanel::update_embedded_layout_constraints() {
    if (!embedded_mode_) {
        target_body_height_ = 0;
        set_available_height_override(-1);
        set_visible_height(default_visible_height_);
        return;
    }
    if (embedded_bounds_.w <= 0 || embedded_bounds_.h <= 0) {
        target_body_height_ = 0;
        set_available_height_override(-1);
        set_visible_height(default_visible_height_);
        return;
    }
    const int padding = DMSpacing::panel_padding();
    const int header_h = show_header() ? DMButton::height() : 0;
    const int header_gap = show_header() ? DMSpacing::header_gap() : 0;
    int available = embedded_bounds_.h - (padding * 2 + header_h + header_gap);
    if (available < 0) {
        available = 0;
    }
    target_body_height_ = available;
    set_visible_height(available);
    set_available_height_override(available);
}

void MapLayersPanel::update(const Input& input, int screen_w, int screen_h) {
    if (!is_visible()) {
        return;
    }
    if (data_dirty_) {
        rebuild_layers();
        data_dirty_ = false;
    }
    if (validation_dirty_) {
        validate_layers();
    }
    update_min_edge_note();
    DockableCollapsible::update(input, screen_w, screen_h);
    if (validation_dirty_) {
        validate_layers();
    }
    if (pending_save_ && !validation_has_errors_) {
        pending_save_ = false;
        perform_save();
    }
}

bool MapLayersPanel::handle_event(const SDL_Event& e) {
    if (!is_visible()) {
        return false;
    }
    return DockableCollapsible::handle_event(e);
}

void MapLayersPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    if (!is_visible()) {
        return;
    }
    DockableCollapsible::render(renderer);
}

bool MapLayersPanel::is_point_inside(int x, int y) const {
    return DockableCollapsible::is_point_inside(x, y);
}

void MapLayersPanel::select_layer(int index) {
    if (index < 0) {
        if (selected_layer_index_ != -1) {
            selected_layer_index_ = -1;
        }
        if (on_layer_selected_) {
            on_layer_selected_(-1);
        }
        recalculate_dependency_highlights();
        force_layer_controls_on_select_ = false;
        notify_side_panel(SidePanel::RoomsList);
        return;
    }

    const int previous_selection = selected_layer_index_;
    int resolved_index = index;
    const int count = static_cast<int>(layer_rows_.size());
    bool found = false;
    for (const auto& row : layer_rows_) {
        if (row.index == index) {
            found = true;
            break;
        }
    }
    if (!found && index >= 0 && index < count) {
        resolved_index = layer_rows_[index].index;
        found = true;
    }
    if (!found) {
        force_layer_controls_on_select_ = false;
        return;
    }

    selected_layer_index_ = resolved_index;
    std::string name;
    if (controller_) {
        if (const nlohmann::json* layer = controller_->layer(selected_layer_index_)) {
            name = layer->value("name", std::string{});
        }
    } else {
        const nlohmann::json& layers = layers_array();
        if (selected_layer_index_ >= 0 && selected_layer_index_ < static_cast<int>(layers.size())) {
            name = layers[selected_layer_index_].value("name", std::string{});
        }
    }
    if (name.empty()) {
        name = "Layer " + std::to_string(selected_layer_index_);
    }
    if (on_layer_selected_) {
        on_layer_selected_(selected_layer_index_);
    }
    const bool notify_controls = force_layer_controls_on_select_ || selected_layer_index_ != previous_selection;
    force_layer_controls_on_select_ = false;
    recalculate_dependency_highlights();
    if (notify_controls) {
        notify_side_panel(SidePanel::LayerControls);
    }
}

void MapLayersPanel::mark_dirty(bool trigger_preview) {
    data_dirty_ = true;
    validation_dirty_ = true;
    if (trigger_preview && preview_widget_) {
        preview_widget_->mark_dirty();
    }
}

void MapLayersPanel::mark_clean() {
    data_dirty_ = false;
    validation_dirty_ = false;
}

void MapLayersPanel::rebuild_layers() {
    sync_min_edge_textbox();
    const nlohmann::json& layers = controller_ ? controller_->layers() : layers_array();
    rebuild_layer_rows_from_json(layers);

    if (selected_layer_index_ >= static_cast<int>(layer_rows_.size())) {
        selected_layer_index_ = layer_rows_.empty() ? -1 : layer_rows_.back().index;
    }

    update_layer_row_geometry();
    validation_dirty_ = true;
    validate_layers();

    if (selected_layer_index_ >= 0) {
        select_layer(selected_layer_index_);
    } else {
        apply_dependency_highlights();
        update_preview_state();
    }

    if (preview_widget_) {
        preview_widget_->mark_dirty();
    }
}

void MapLayersPanel::rebuild_layer_rows_from_json(const nlohmann::json& layers) {
    layer_rows_.clear();
    hovered_delete_layer_index_ = -1;
    if (!layers.is_array()) {
        return;
    }
    layer_rows_.reserve(layers.size());

    for (std::size_t i = 0; i < layers.size(); ++i) {
        LayerRow row;
        row.index = static_cast<int>(i);
        row.rect = SDL_Rect{0, 0, 0, 0};
        row.invalid = false;
        row.warning = false;
        row.dependency_highlight = false;
        row.deletable = (i != 0);

        const auto& layer_json = layers[i];
        std::string name;
        if (layer_json.is_object()) {
            name = layer_json.value("name", std::string());
        }
        if (name.empty()) {
            name = "Layer " + std::to_string(i);
        }
        row.name = std::move(name);

        if (layer_json.is_object()) {
            int room_count = 0;
            std::string first_room_name;
            const auto rooms_it = layer_json.find("rooms");
            if (rooms_it != layer_json.end() && rooms_it->is_array()) {
                room_count = static_cast<int>(rooms_it->size());
                if (!rooms_it->empty()) {
                    const auto& first_entry = (*rooms_it)[0];
                    if (first_entry.is_object()) {
                        first_room_name = first_entry.value("name", std::string());
                    } else if (first_entry.is_string()) {
                        first_room_name = first_entry.get<std::string>();
                    }
                }
            }

            const int min_rooms = layer_json.value("min_rooms", -1);
            const int max_rooms = layer_json.value("max_rooms", -1);

            std::ostringstream summary;
            if (room_count <= 0) {
                summary << "No rooms configured";
            } else {
                summary << room_count << (room_count == 1 ? " room" : " rooms");
            }

            if (min_rooms >= 0 || max_rooms >= 0) {
                int derived_min = std::max(0, min_rooms);
                int derived_max = std::max(derived_min, max_rooms);
                summary << " • target " << derived_min << "-" << derived_max;
            }

            if (i == 0) {
                if (!first_room_name.empty()) {
                    summary << " • center: " << first_room_name;
                } else {
                    summary << " • center";
                }
            }

            row.summary = summary.str();
        } else {
            row.summary = "Layer data missing";
        }

        layer_rows_.push_back(std::move(row));
    }
}

void MapLayersPanel::update_layer_row_geometry() {
    if (!list_widget_) {
        return;
    }
    SDL_Rect area = list_widget_->rect();
    const int padding = DMSpacing::small_gap();
    const int gap = DMSpacing::small_gap();
    int y = area.y + padding;
    const int width = std::max(0, area.w - padding * 2);
    for (auto& row : layer_rows_) {
        row.rect = SDL_Rect{area.x + padding, y, width, kRowHeight};
        const int available_height = std::max(0, row.rect.h - padding * 2);
        const int button_size = std::max(0, std::min(kLayerDeleteButtonSize, available_height));
        if (!row.deletable) {
            row.delete_button_rect = SDL_Rect{row.rect.x + row.rect.w, row.rect.y, 0, 0};
        } else if (button_size > 0) {
            const int button_x = std::max(row.rect.x + padding, row.rect.x + row.rect.w - padding - button_size);
            const int button_y = row.rect.y + (row.rect.h - button_size) / 2;
            row.delete_button_rect = SDL_Rect{button_x, button_y, button_size, button_size};
        } else {
            row.delete_button_rect = SDL_Rect{row.rect.x + row.rect.w, row.rect.y, 0, 0};
        }
        y += kRowHeight + gap;
    }
}

int MapLayersPanel::list_height_for_width(int w) const {
    const int padding = DMSpacing::small_gap();
    const int gap = DMSpacing::small_gap();
    int base_total = padding * 2;
    if (!layer_rows_.empty()) {
        base_total += static_cast<int>(layer_rows_.size()) * kRowHeight;
        if (layer_rows_.size() > 1) {
            base_total += static_cast<int>(layer_rows_.size() - 1) * gap;
        }
    } else {
        base_total = kMinimumListHeight;
    }

    int required = base_total;
    if (target_body_height_ > 0) {
        const int row_gap = DMSpacing::item_gap();
        int rows_present = 0;
        int other_heights = 0;

        rows_present += 1;
        other_heights += DMButton::height();

        rows_present += 1;

        if (preview_widget_) {
            rows_present += 1;
            other_heights += preview_widget_->height_for_width(w);
        }
        if (min_edge_widget_) {
            rows_present += 1;
            other_heights += min_edge_widget_->height_for_width(w);
        }
        if (validation_widget_) {
            rows_present += 1;
            other_heights += validation_summary_height(w);
        }

        const int gap_total = std::max(0, rows_present - 1) * row_gap;
        const int needed = target_body_height_ - (other_heights + gap_total);
        if (needed > required) {
            required = needed;
        }
    }

    return std::max(kMinimumListHeight, required);
}

void MapLayersPanel::render_layers_list(SDL_Renderer* renderer) const {
    if (!renderer || !list_widget_) {
        return;
    }
    SDL_Rect area = list_widget_->rect();
    if (area.w <= 0 || area.h <= 0) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const SDL_Color panel_bg = DMStyles::PanelBG();
    SDL_SetRenderDrawColor(renderer, panel_bg.r, panel_bg.g, panel_bg.b, panel_bg.a);
    sdl_render::FillRect(renderer, &area);

    const SDL_Color border = DMStyles::Border();
    sdl_render::Rect(renderer, &area);

    const int padding = DMSpacing::small_gap();
    const DMLabelStyle& label_style = DMStyles::Label();

    if (layer_rows_.empty()) {
        const std::string message = "No layers configured. Add or duplicate a layer to begin.";
        SDL_Point size = MeasureLabelText(label_style, message);
        int text_x = area.x + padding;
        int text_y = area.y + padding;
        if (size.y < area.h) {
            text_y = area.y + (area.h - size.y) / 2;
        }
        DrawLabelText(renderer, message, text_x, text_y, label_style);
        return;
    }

    const DMLabelStyle& summary_style = summary_label_style();
    const SDL_Color selection_outline = DMStyles::AccentButton().border;
    const SDL_Color dependency_outline = DMStyles::AccentButton().hover_bg;
    const int accent_width = 4;

    for (const auto& row : layer_rows_) {
        SDL_Rect rect = row.rect;
        if (rect.w <= 0 || rect.h <= 0) {
            continue;
        }

        const bool selected = (row.index == selected_layer_index_);
        const bool hovered = (row.index == hovered_layer_index_);
        const bool dependency = row.dependency_highlight;
        const bool dragging = dragging_layer_active_ && row.index == dragging_layer_index_;

        SDL_Color fill = severity_fill(row.invalid, row.warning, selected);
        if (dependency && !selected) {
            fill = lighten(fill, 0.12f);
        }
        if (hovered && !selected) {
            fill = lighten(fill, 0.08f);
        }
        if (dragging && drag_moved_) {
            fill = lighten(fill, 0.18f);
        }

        SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
        sdl_render::FillRect(renderer, &rect);

        SDL_Color outline = severity_color(row.invalid, row.warning, selected || dependency);
        if (selected) {
            outline = selection_outline;
        } else if (dependency && !row.invalid && !row.warning) {
            outline = dependency_outline;
        } else if (hovered) {
            outline = lighten(outline, 0.2f);
        }
        SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, outline.a);
        sdl_render::Rect(renderer, &rect);

        SDL_Rect accent{rect.x, rect.y, accent_width, rect.h};
        SDL_Color accent_color = outline;
        if (selected) {
            accent_color = DMStyles::AccentButton().bg;
        } else if (row.invalid) {
            accent_color = error_color();
        } else if (row.warning) {
            accent_color = warning_color();
        } else if (dependency) {
            accent_color = dependency_outline;
        }
        SDL_SetRenderDrawColor(renderer, accent_color.r, accent_color.g, accent_color.b, accent_color.a);
        sdl_render::FillRect(renderer, &accent);

        const int text_x = rect.x + accent_width + padding;
        const int text_y = rect.y + padding;
        DrawLabelText(renderer, row.name, text_x, text_y, label_style);

        if (!row.summary.empty()) {
            SDL_Point summary_size = MeasureLabelText(summary_style, row.summary);
            int summary_y = rect.y + rect.h - summary_size.y - padding;
            DrawLabelText(renderer, row.summary, text_x, summary_y, summary_style);
        }

        SDL_Rect delete_rect = row.delete_button_rect;
        if (delete_rect.w > 0 && delete_rect.h > 0) {
            const bool delete_hovered = (hovered_delete_layer_index_ == row.index);
            SDL_Color delete_border = error_color();
            SDL_Color delete_fill = darken(delete_border, 0.35f);
            if (delete_hovered) {
                delete_fill = lighten(delete_border, 0.25f);
            } else if (selected) {
                delete_fill = lighten(delete_fill, 0.12f);
            }

            SDL_SetRenderDrawColor(renderer, delete_fill.r, delete_fill.g, delete_fill.b, delete_fill.a);
            sdl_render::FillRect(renderer, &delete_rect);

            SDL_Color delete_outline = delete_border;
            if (delete_hovered) {
                delete_outline = lighten(delete_outline, 0.1f);
            }
            SDL_SetRenderDrawColor(renderer, delete_outline.r, delete_outline.g, delete_outline.b, delete_outline.a);
            sdl_render::Rect(renderer, &delete_rect);

            const int cross_pad = std::max(3, delete_rect.w / 4);
            SDL_Color cross_color{255, 255, 255, 255};
            SDL_SetRenderDrawColor(renderer, cross_color.r, cross_color.g, cross_color.b, cross_color.a);
            SDL_RenderLine(renderer, delete_rect.x + cross_pad, delete_rect.y + cross_pad, delete_rect.x + delete_rect.w - cross_pad - 1, delete_rect.y + delete_rect.h - cross_pad - 1);
            SDL_RenderLine(renderer, delete_rect.x + delete_rect.w - cross_pad - 1, delete_rect.y + cross_pad, delete_rect.x + cross_pad, delete_rect.y + delete_rect.h - cross_pad - 1);
        }

        const std::string level = std::string{"Lvl "} + std::to_string(row.index);
        SDL_Point level_size = MeasureLabelText(summary_style, level);
        int level_right_edge = rect.x + rect.w - padding;
        if (delete_rect.w > 0) {
            level_right_edge = delete_rect.x - padding;
        }
        level_right_edge = std::max(level_right_edge, text_x + level_size.x);
        int level_x = level_right_edge - level_size.x;
        int level_y = rect.y + padding;
        DrawLabelText(renderer, level, level_x, level_y, summary_style);

        if (row.invalid || row.warning) {
            SDL_Color dot = row.invalid ? error_color() : warning_color();
            int badge_right = level_x - padding / 2;
            int badge_x = std::max(text_x, badge_right - 8);
            SDL_Rect badge{badge_x, rect.y + rect.h / 2 - 4, 8, 8};
            SDL_SetRenderDrawColor(renderer, dot.r, dot.g, dot.b, dot.a);
            sdl_render::FillRect(renderer, &badge);
        }
    }

    if (dragging_layer_active_ && drag_moved_) {
        int slot = std::clamp(drop_target_slot_, 0, static_cast<int>(layer_rows_.size()));
        int indicator_y = 0;
        if (slot < static_cast<int>(layer_rows_.size())) {
            indicator_y = layer_rows_[slot].rect.y;
        } else if (!layer_rows_.empty()) {
            indicator_y = layer_rows_.back().rect.y + layer_rows_.back().rect.h;
        }
        SDL_Rect drop_rect{area.x + padding, indicator_y - kDropIndicatorThickness / 2,
                           area.w - padding * 2, kDropIndicatorThickness};
        SDL_Color drop_color = DMStyles::AccentButton().bg;
        SDL_SetRenderDrawColor(renderer, drop_color.r, drop_color.g, drop_color.b, drop_color.a);
        sdl_render::FillRect(renderer, &drop_rect);
    }
}

void MapLayersPanel::on_layers_list_mouse_down(int index, int mouse_y) {
    if (index == 0) {
        select_layer(index);
        dragging_layer_active_ = false;
        drag_moved_ = false;
        dragging_layer_index_ = -1;
        dragging_start_slot_ = -1;
        drop_target_slot_ = -1;
        drag_start_mouse_y_ = mouse_y;
        return;
    }
    dragging_layer_active_ = true;
    drag_moved_ = false;
    dragging_layer_index_ = index;
    dragging_start_slot_ = find_visual_position(index);
    drop_target_slot_ = dragging_start_slot_;
    drag_start_mouse_y_ = mouse_y;
    if (index >= 0) {
        select_layer(index);
    }
}

void MapLayersPanel::on_layers_list_mouse_motion(int mouse_y, Uint32 buttons) {
    if (!dragging_layer_active_) {
        return;
    }
    if ((buttons & SDL_BUTTON_LMASK) == 0) {
        cancel_drag();
        return;
    }
    if (!drag_moved_ && std::abs(mouse_y - drag_start_mouse_y_) > 4) {
        drag_moved_ = true;
    }
    if (!drag_moved_) {
        return;
    }
    drop_target_slot_ = drop_slot_for_position(mouse_y);
}

void MapLayersPanel::on_layers_list_mouse_up(int mouse_y, Uint8 button) {
    if (!dragging_layer_active_) {
        if (button == SDL_BUTTON_LEFT && hovered_layer_index_ >= 0) {
            select_layer(hovered_layer_index_);
        }
        return;
    }

    const bool was_dragging = drag_moved_;
    const int start_slot = dragging_start_slot_;
    const int original_index = dragging_layer_index_;
    int target_slot = drop_target_slot_;

    dragging_layer_active_ = false;
    drag_moved_ = false;
    dragging_layer_index_ = -1;
    dragging_start_slot_ = -1;
    drop_target_slot_ = -1;

    if (button != SDL_BUTTON_LEFT) {
        return;
    }

    if (!was_dragging || start_slot < 0) {
        if (hovered_layer_index_ >= 0) {
            select_layer(hovered_layer_index_);
        } else if (original_index >= 0) {
            select_layer(original_index);
        }
        return;
    }

    if (layer_rows_.empty()) {
        return;
    }

    if (target_slot < 0) {
        target_slot = start_slot;
    }

    if (target_slot == start_slot || target_slot == start_slot + 1) {
        select_layer(original_index);
        return;
    }

    int to_slot = target_slot;
    if (to_slot > start_slot) {
        to_slot -= 1;
    }
    to_slot = std::clamp(to_slot, 0, static_cast<int>(layer_rows_.size()) - 1);
    if (to_slot == 0 && static_cast<int>(layer_rows_.size()) > 1) {
        to_slot = 1;
    }

    bool changed = false;
    if (controller_) {
        changed = controller_->reorder_layer(start_slot, to_slot);
    } else {
        nlohmann::json& layers = layers_array();
        if (layers.is_array() && !layers.empty() && start_slot >= 0 &&
            start_slot < static_cast<int>(layers.size()) && to_slot >= 0 &&
            to_slot < static_cast<int>(layers.size())) {
            nlohmann::json layer = layers[start_slot];
            layers.erase(layers.begin() + start_slot);
            layers.insert(layers.begin() + to_slot, std::move(layer));
            changed = true;
        }
    }

    if (changed) {
        selected_layer_index_ = to_slot;
        mark_dirty(false);
        rebuild_layers();
        data_dirty_ = false;
        trigger_save();
    } else {
        if (original_index >= 0) {
            select_layer(original_index);
        }
    }
}

void MapLayersPanel::cancel_drag() {
    dragging_layer_active_ = false;
    drag_moved_ = false;
    dragging_layer_index_ = -1;
    dragging_start_slot_ = -1;
    drop_target_slot_ = -1;
}

int MapLayersPanel::drop_slot_for_position(int y) const {
    int slot = 0;
    for (const auto& row : layer_rows_) {
        int midpoint = row.rect.y + row.rect.h / 2;
        if (y < midpoint) {
            return slot;
        }
        ++slot;
    }
    return slot;
}

int MapLayersPanel::find_visual_position(int layer_index) const {
    for (std::size_t i = 0; i < layer_rows_.size(); ++i) {
        if (layer_rows_[i].index == layer_index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MapLayersPanel::apply_dependency_highlights() {
    std::unordered_set<int> highlight_set(dependency_highlight_layers_.begin(), dependency_highlight_layers_.end());
    for (auto& row : layer_rows_) {
        row.dependency_highlight = highlight_set.find(row.index) != highlight_set.end();
    }
}

bool MapLayersPanel::validate_layers() {
    if (!validation_dirty_) {
        return !validation_has_errors_;
    }

    validation_dirty_ = false;
    validation_lines_.clear();
    invalid_layers_.clear();
    warning_layers_.clear();
    dependency_highlight_layers_.clear();
    layer_dependency_children_.clear();
    layer_dependency_parents_.clear();
    root_room_summary_.clear();
    estimated_map_radius_ = 0.0;

    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    const nlohmann::json& layers = controller_ ? controller_->layers() : layers_array();
    if (!layers.is_array() || layers.empty()) {
        errors.emplace_back("At least one layer is required for map generation.");
        validation_has_errors_ = true;
        validation_has_warnings_ = false;
        update_validation_summary_layout(errors, warnings);
        apply_dependency_highlights();
        update_preview_state();
        return false;
    }

    const std::size_t layer_count = layers.size();
    layer_dependency_children_.assign(layer_count, {});
    layer_dependency_parents_.assign(layer_count, {});
    std::vector<std::vector<std::string>> required_children_names(layer_count);

    std::unordered_set<std::string> layer_names;
    std::unordered_map<std::string, int> room_to_layer;
    std::unordered_map<std::string, int> room_occurrences;

    for (std::size_t i = 0; i < layer_count; ++i) {
        const auto& layer = layers[i];
        const int index = static_cast<int>(i);
        bool layer_has_error = false;

        if (!layer.is_object()) {
            errors.emplace_back("Layer " + std::to_string(i) + " is not an object.");
            invalid_layers_.push_back(index);
            continue;
        }

        std::string layer_name = trimmed(layer.value("name", std::string()));
        const std::string layer_label =
            layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name;
        if (layer_name.empty()) {
            errors.emplace_back("Layer " + std::to_string(i) + " is missing a name.");
            invalid_layers_.push_back(index);
            layer_has_error = true;
        } else {
            if (!layer_names.insert(layer_name).second) {
                warnings.emplace_back("Layer name '" + layer_name + "' is duplicated.");
                warning_layers_.push_back(index);
            }
        }

        const auto rooms_it = layer.find("rooms");
        if (rooms_it == layer.end() || !rooms_it->is_array()) {
            errors.emplace_back("Layer '" + layer_label + "' is missing its room list.");
            invalid_layers_.push_back(index);
            continue;
        }

        const auto& rooms_array = *rooms_it;
        if (rooms_array.empty()) {
            if (i == 0) {
                errors.emplace_back("Layer 0 must include exactly one center-room candidate.");
                invalid_layers_.push_back(index);
                layer_has_error = true;
            } else {
                warnings.emplace_back("Layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' does not contain any rooms.");
                warning_layers_.push_back(index);
            }
        } else if (i == 0) {
            if (rooms_array.size() != 1) {
                errors.emplace_back("Layer '" + layer_label + "' must contain exactly one room candidate.");
                invalid_layers_.push_back(index);
                layer_has_error = true;
            } else {
                const auto& spawn_entry = rooms_array.front();
                if (!spawn_entry.is_object()) {
                    errors.emplace_back("Layer '" + layer_label + "' has an invalid center-room entry.");
                    invalid_layers_.push_back(index);
                    layer_has_error = true;
                } else {
                    const int min_instances = spawn_entry.value("min_instances", 0);
                    const int max_instances = spawn_entry.value("max_instances", 0);
                    if (min_instances != 1 || max_instances != 1) {
                        errors.emplace_back("Layer '" + layer_label + "' center-room candidate must allow exactly one instance.");
                        invalid_layers_.push_back(index);
                        layer_has_error = true;
                    }
                }
            }
        }

        int min_rooms = layer.value("min_rooms", 0);
        int max_rooms = layer.value("max_rooms", 0);
        if (min_rooms < 0) {
            min_rooms = 0;
        }
        if (max_rooms < min_rooms) {
            errors.emplace_back("Layer '" + layer_label + "' has min_rooms greater than max_rooms.");
            invalid_layers_.push_back(index);
            layer_has_error = true;
        }
        if (i == 0 && (min_rooms != 1 || max_rooms != 1)) {
            errors.emplace_back("Layer '" + layer_label + "' must require exactly one room.");
            invalid_layers_.push_back(index);
            layer_has_error = true;
        }

        for (const auto& candidate : rooms_array) {
            if (!candidate.is_object()) {
                warnings.emplace_back("Layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' has a room entry that is not an object.");
                warning_layers_.push_back(index);
                continue;
            }
            std::string room_name = trimmed(candidate.value("name", std::string()));
            if (room_name.empty()) {
                errors.emplace_back("Layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' has a room with an empty name.");
                invalid_layers_.push_back(index);
                layer_has_error = true;
                continue;
            }
            room_occurrences[room_name]++;
            if (!room_to_layer.count(room_name)) {
                room_to_layer[room_name] = index;
            }
            if (i == 0 && root_room_summary_.empty()) {
                root_room_summary_ = room_name;
            }

            int max_instances = candidate.value("max_instances", 1);
            if (max_instances <= 0) {
                warnings.emplace_back("Room '" + room_name + "' in layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' has max_instances <= 0.");
                warning_layers_.push_back(index);
            }

            const auto required_it = candidate.find("required_children");
            if (required_it != candidate.end() && required_it->is_array()) {
                for (const auto& child_entry : *required_it) {
                    if (!child_entry.is_string()) {
                        warnings.emplace_back("Room '" + room_name + "' in layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' has a non-string required child entry.");
                        warning_layers_.push_back(index);
                        continue;
                    }
                    std::string child_name = trimmed(child_entry.get<std::string>());
                    if (child_name.empty()) {
                        warnings.emplace_back("Room '" + room_name + "' in layer '" + (layer_name.empty() ? std::string("Layer ") + std::to_string(i) : layer_name) + "' has a blank required child name.");
                        warning_layers_.push_back(index);
                        continue;
                    }
                    required_children_names[i].push_back(child_name);
                }
            }
        }

        if (layer_has_error) {
            invalid_layers_.push_back(index);
        }
    }

    auto deduplicate_indices = [](std::vector<int>& vec) {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
};

    deduplicate_indices(invalid_layers_);
    deduplicate_indices(warning_layers_);

    for (const auto& entry : room_occurrences) {
        if (entry.second > 1) {
            warnings.emplace_back("Room '" + entry.first + "' appears in multiple layers.");
        }
    }

    for (std::size_t i = 0; i < required_children_names.size(); ++i) {
        std::unordered_set<int> unique_children;
        for (const std::string& child_name : required_children_names[i]) {
            auto it = room_to_layer.find(child_name);
            const int index = static_cast<int>(i);
            const std::string layer_label = (i < layer_rows_.size() ? layer_rows_[i].name : std::string("Layer ") + std::to_string(i));
            if (it == room_to_layer.end()) {
                errors.emplace_back("Layer '" + layer_label + "' references unknown room '" + child_name + "'.");
                invalid_layers_.push_back(index);
                continue;
            }
            const int child_layer = it->second;
            if (child_layer <= static_cast<int>(i)) {
                errors.emplace_back("Layer '" + layer_label + "' requires '" + child_name + "' from an earlier or same layer.");
                invalid_layers_.push_back(index);
                continue;
            }
            if (unique_children.insert(child_layer).second) {
                layer_dependency_children_[i].push_back(child_layer);
                if (child_layer >= 0 && child_layer < static_cast<int>(layer_dependency_parents_.size())) {
                    layer_dependency_parents_[child_layer].push_back(static_cast<int>(i));
                }
            }
        }
    }

    deduplicate_indices(invalid_layers_);
    deduplicate_indices(warning_layers_);
    for (auto& children : layer_dependency_children_) {
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
    }
    for (auto& parents : layer_dependency_parents_) {
        std::sort(parents.begin(), parents.end());
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
    }

    validation_has_errors_ = !errors.empty();
    validation_has_warnings_ = !warnings.empty();

    if (map_info_ && map_info_->is_object()) {
        estimated_map_radius_ = map_layers::map_radius_from_map_info(*map_info_);
    } else {
        estimated_map_radius_ = 0.0;
    }

    for (auto& row : layer_rows_) {
        row.invalid = std::binary_search(invalid_layers_.begin(), invalid_layers_.end(), row.index);
        row.warning = std::binary_search(warning_layers_.begin(), warning_layers_.end(), row.index);
    }

    update_validation_summary_layout(errors, warnings);
    recalculate_dependency_highlights();
    return !validation_has_errors_;
}

void MapLayersPanel::recalculate_dependency_highlights() {
    dependency_highlight_layers_.clear();
    const int layer_count = static_cast<int>(layer_dependency_children_.size());
    if (selected_layer_index_ < 0 || selected_layer_index_ >= layer_count) {
        apply_dependency_highlights();
        update_preview_state();
        return;
    }

    std::unordered_set<int> highlight_set;
    if (selected_layer_index_ >= 0 && selected_layer_index_ < layer_count) {
        for (int child : layer_dependency_children_[selected_layer_index_]) {
            if (child != selected_layer_index_) {
                highlight_set.insert(child);
            }
        }
    }
    if (selected_layer_index_ >= 0 &&
        selected_layer_index_ < static_cast<int>(layer_dependency_parents_.size())) {
        for (int parent : layer_dependency_parents_[selected_layer_index_]) {
            if (parent != selected_layer_index_) {
                highlight_set.insert(parent);
            }
        }
    }

    dependency_highlight_layers_.assign(highlight_set.begin(), highlight_set.end());
    std::sort(dependency_highlight_layers_.begin(), dependency_highlight_layers_.end());
    apply_dependency_highlights();
    update_preview_state();
}

void MapLayersPanel::perform_save() {
    bool ok = false;
    if (controller_) {
        ok = controller_->save();
    }
    if (!ok && on_save_) {
        ok = on_save_();
    }
    save_blocked_ = !ok;
}

void MapLayersPanel::update_preview_state() {
    if (!preview_widget_) {
        return;
    }
    preview_widget_->set_selected_layer(selected_layer_index_);
    preview_widget_->set_layer_diagnostics(invalid_layers_, warning_layers_, dependency_highlight_layers_);
}

int MapLayersPanel::validation_summary_height(int) const {
    if (validation_lines_.empty()) {
        return validation_label_style().font_size + DMSpacing::small_gap() * 2;
    }
    const int line_height = validation_label_style().font_size + DMSpacing::small_gap();
    return static_cast<int>(validation_lines_.size()) * line_height + DMSpacing::small_gap();
}

void MapLayersPanel::render_validation_summary(SDL_Renderer* renderer, const SDL_Rect& rect) const {
    if (!renderer) {
        return;
    }
    SDL_Rect area = rect;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 18, 26, 42, 230);
    sdl_render::FillRect(renderer, &area);
    SDL_SetRenderDrawColor(renderer, DMStyles::Border().r, DMStyles::Border().g, DMStyles::Border().b, DMStyles::Border().a);
    sdl_render::Rect(renderer, &area);

    int y = area.y + DMSpacing::small_gap();
    const DMLabelStyle base_style = validation_label_style();
    for (const auto& line : validation_lines_) {
        DMLabelStyle style = base_style;
        style.color = line.color;
        DrawLabelText(renderer, line.text, area.x + DMSpacing::small_gap(), y, style);
        y += base_style.font_size + DMSpacing::small_gap();
    }
}

void MapLayersPanel::update_validation_summary_layout(const std::vector<std::string>& errors,
                                                      const std::vector<std::string>& warnings) {
    validation_lines_.clear();

    if (!errors.empty()) {
        validation_lines_.push_back({"Resolve the highlighted issues before saving.", error_color()});
        const std::size_t limit = std::min<std::size_t>(errors.size(), 3);
        for (std::size_t i = 0; i < limit; ++i) {
            validation_lines_.push_back({"• " + errors[i], error_color()});
        }
        if (errors.size() > limit) {
            validation_lines_.push_back({"• " + std::to_string(errors.size() - limit) + " more issue(s)...", error_color()});
        }
    } else if (!warnings.empty()) {
        validation_lines_.push_back({"Warnings detected. Review before publishing.", warning_color()});
        const std::size_t limit = std::min<std::size_t>(warnings.size(), 3);
        for (std::size_t i = 0; i < limit; ++i) {
            validation_lines_.push_back({"• " + warnings[i], warning_color()});
        }
        if (warnings.size() > limit) {
            validation_lines_.push_back({"• " + std::to_string(warnings.size() - limit) + " additional warning(s)...",
                                        warning_color()});
        }
    } else {
        validation_lines_.push_back({"Layers ready. No validation issues detected.", success_color()});
    }

    if (!root_room_summary_.empty()) {
        validation_lines_.push_back({"Center room: " + root_room_summary_, info_color()});
    }

    if (estimated_map_radius_ > 0.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << estimated_map_radius_;
        validation_lines_.push_back({"Estimated map radius ≈ " + oss.str(), info_color()});
    }

    if (save_blocked_) {
        validation_lines_.push_back({"Save paused until issues are resolved.", error_color()});
    }

    validation_lines_.push_back({"Tip: Drag layers to reorder. Use Duplicate to branch quickly.", info_color()});
}

void MapLayersPanel::trigger_save() {
    if (!validate_layers()) {
        pending_save_ = true;
        save_blocked_ = true;
        return;
    }
    pending_save_ = false;
    save_blocked_ = false;
    perform_save();
}

void MapLayersPanel::ensure_listener() {
    if (!controller_ || controller_listener_id_ != 0) {
        return;
    }
    controller_listener_id_ = controller_->add_listener([this]() {
        this->mark_dirty();
    });
}

void MapLayersPanel::remove_listener() {
    if (controller_ && controller_listener_id_ != 0) {
        controller_->remove_listener(controller_listener_id_);
    }
    controller_listener_id_ = 0;
}

void MapLayersPanel::notify_header_visibility() const {
    if (header_visibility_callback_) {
        header_visibility_callback_(is_visible());
    }
}

void MapLayersPanel::notify_side_panel(SidePanel panel) const {
    if (side_panel_callback_) {
        side_panel_callback_(panel);
    }
}

void MapLayersPanel::set_hovered_layer(int index) {
    hovered_layer_index_ = index;
}

void MapLayersPanel::set_hovered_delete_layer(int index) {
    hovered_delete_layer_index_ = index;
}

void MapLayersPanel::on_delete_layer_clicked(int index) {
    if (delete_layer_at(index)) {
        hovered_layer_index_ = -1;
        hovered_delete_layer_index_ = -1;
    }
}

bool MapLayersPanel::delete_layer_at(int index) {
    if (index < 0) {
        return false;
    }
    if (index == 0) {
        return false;
    }

    bool removed = false;
    if (controller_) {
        removed = controller_->delete_layer(index);
    } else {
        nlohmann::json& layers = layers_array();
        if (layers.is_array() && index >= 0 && index < static_cast<int>(layers.size())) {
            layers.erase(layers.begin() + index);
            removed = true;
        }
    }

    if (!removed) {
        return false;
    }

    if (selected_layer_index_ == index) {
        selected_layer_index_ = -1;
    } else if (selected_layer_index_ > index) {
        --selected_layer_index_;
    }

    hovered_layer_index_ = -1;
    hovered_delete_layer_index_ = -1;

    mark_dirty();
    trigger_save();
    return true;
}

void MapLayersPanel::clear_hover() {
    hovered_layer_index_ = -1;
    hovered_delete_layer_index_ = -1;
}

const nlohmann::json& MapLayersPanel::layers_array() const {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    if (!map_info_ || !map_info_->is_object()) {
        return kEmpty;
    }
    auto it = map_info_->find("map_layers");
    if (it == map_info_->end() || !it->is_array()) {
        return kEmpty;
    }
    return *it;
}

nlohmann::json& MapLayersPanel::layers_array() {
    static nlohmann::json dummy = nlohmann::json::array();
    if (!map_info_ || !map_info_->is_object()) {
        dummy = nlohmann::json::array();
        return dummy;
    }
    if (!map_info_->contains("map_layers") || !(*map_info_)["map_layers"].is_array()) {
        (*map_info_)["map_layers"] = nlohmann::json::array();
    }
    return (*map_info_)["map_layers"];
}

void MapLayersPanel::sync_min_edge_textbox() {
    int value = map_layers::kDefaultMinEdgeDistance;
    if (controller_) {
        value = static_cast<int>(std::lround(controller_->min_edge_distance()));
    } else if (map_info_) {
        value = static_cast<int>(std::lround(map_layers::min_edge_distance_from_map_manifest(*map_info_)));
    }
    value = std::clamp(value, 0, static_cast<int>(map_layers::kMinEdgeDistanceMax));
    min_edge_value_ = value;
    last_valid_min_edge_text_ = std::to_string(value);
    if (min_edge_textbox_ && !min_edge_textbox_->is_editing()) {
        min_edge_textbox_->set_value(last_valid_min_edge_text_);
    }
    if (min_edge_widget_) {
        min_edge_widget_->mark_layout_dirty();
    }
}

bool MapLayersPanel::handle_min_edge_event(const SDL_Event& e) {
    if (!min_edge_textbox_) {
        return false;
    }
    const bool was_editing = min_edge_textbox_->is_editing();
    const bool changed = min_edge_textbox_->handle_event(e);
    const bool now_editing = min_edge_textbox_->is_editing();
    if (changed && now_editing) {
        on_min_edge_text_changed();
    }
    if (was_editing && !now_editing) {
        on_min_edge_edit_finished();
    }
    return changed || was_editing != now_editing;
}

void MapLayersPanel::on_min_edge_text_changed() {
    if (!min_edge_textbox_) {
        return;
    }
    clear_min_edge_note();
    if (min_edge_widget_) {
        min_edge_widget_->mark_layout_dirty();
    }
}

void MapLayersPanel::on_min_edge_edit_finished() {
    if (!min_edge_textbox_) {
        return;
    }
    std::string raw_value = min_edge_textbox_->value();
    std::string trimmed_value = trimmed(raw_value);
    if (trimmed_value.empty()) {
        min_edge_textbox_->set_value(last_valid_min_edge_text_);
        show_min_edge_note("Enter a number between 0 and 10000.", error_color());
        if (min_edge_widget_) {
            min_edge_widget_->mark_layout_dirty();
        }
        return;
    }
    int parsed = 0;
    const char* begin = trimmed_value.data();
    const char* end = begin + trimmed_value.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        min_edge_textbox_->set_value(last_valid_min_edge_text_);
        show_min_edge_note("Enter a number between 0 and 10000.", error_color());
        if (min_edge_widget_) {
            min_edge_widget_->mark_layout_dirty();
        }
        return;
    }
    int clamped = std::clamp(parsed, 0, static_cast<int>(map_layers::kMinEdgeDistanceMax));
    if (clamped != min_edge_value_) {
        apply_min_edge_value(clamped);
    }
    std::string normalized = std::to_string(clamped);
    if (normalized != raw_value) {
        min_edge_textbox_->set_value(normalized);
    }
    last_valid_min_edge_text_ = normalized;
    if (clamped != parsed) {
        show_min_edge_note("Value clamped to 0–10000.", warning_color());
    } else {
        clear_min_edge_note();
    }
    if (min_edge_widget_) {
        min_edge_widget_->mark_layout_dirty();
    }
}

void MapLayersPanel::apply_min_edge_value(int value) {
    value = std::clamp(value, 0, static_cast<int>(map_layers::kMinEdgeDistanceMax));
    if (value == min_edge_value_) {
        return;
    }
    min_edge_value_ = value;
    last_valid_min_edge_text_ = std::to_string(value);
    if (controller_) {
        controller_->set_min_edge_distance(static_cast<double>(value));
    } else if (map_info_ && map_info_->is_object()) {
        (*map_info_)["map_layers_settings"]["min_edge_distance"] = value;
        mark_dirty();
    }
    if (preview_widget_) {
        preview_widget_->mark_dirty();
    }
    validation_dirty_ = true;
    clear_min_edge_note();
    trigger_save();
}

void MapLayersPanel::show_min_edge_note(const std::string& message, SDL_Color color) {
    min_edge_note_ = message;
    min_edge_note_color_ = color;
    min_edge_note_expiration_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (min_edge_widget_) {
        min_edge_widget_->mark_layout_dirty();
    }
}

void MapLayersPanel::clear_min_edge_note() {
    if (min_edge_note_.empty()) {
        return;
    }
    min_edge_note_.clear();
    min_edge_note_color_ = DMStyles::Label().color;
    if (min_edge_widget_) {
        min_edge_widget_->mark_layout_dirty();
    }
}

void MapLayersPanel::update_min_edge_note() {
    if (min_edge_note_.empty()) {
        return;
    }
    if (min_edge_note_expiration_ == std::chrono::steady_clock::time_point{}) {
        return;
    }
    if (std::chrono::steady_clock::now() >= min_edge_note_expiration_) {
        clear_min_edge_note();
        min_edge_note_expiration_ = std::chrono::steady_clock::time_point{};
    }
}

bool MapLayersPanel::min_edge_note_visible() const {
    return !min_edge_note_.empty();
}

int MapLayersPanel::min_edge_widget_height_for_width(int w) const {
    const int padding = DMSpacing::small_gap();
    const int inner_width = std::max(0, w - padding * 2);
    int height = padding * 2;
    if (min_edge_textbox_) {
        height += min_edge_textbox_->preferred_height(inner_width);
    } else {
        height += DMTextBox::height();
    }
    if (min_edge_note_visible()) {
        height += DMStyles::Label().font_size + DMSpacing::small_gap();
    }
    return height;
}

void MapLayersPanel::layout_min_edge_input(const SDL_Rect& bounds) {
    if (!min_edge_textbox_) {
        return;
    }
    const int padding = DMSpacing::small_gap();
    const int inner_width = std::max(0, bounds.w - padding * 2);
    const int box_height = min_edge_textbox_->preferred_height(inner_width);
    SDL_Rect text_rect{bounds.x + padding, bounds.y + padding, inner_width, box_height};
    min_edge_textbox_->set_rect(text_rect);
    if (min_edge_note_visible()) {
        int note_y = text_rect.y + text_rect.h + DMSpacing::small_gap();
        min_edge_note_rect_ = SDL_Rect{text_rect.x, note_y, inner_width, DMStyles::Label().font_size};
    } else {
        min_edge_note_rect_ = SDL_Rect{text_rect.x, text_rect.y + text_rect.h, inner_width, 0};
    }
}

void MapLayersPanel::render_min_edge_input(SDL_Renderer* renderer, const SDL_Rect&) const {
    if (min_edge_textbox_) {
        min_edge_textbox_->render(renderer);
    }
    if (min_edge_note_visible() && min_edge_note_rect_.w > 0) {
        DMLabelStyle style = DMStyles::Label();
        style.color = min_edge_note_color_;
        DrawLabelText(renderer, min_edge_note_, min_edge_note_rect_.x, min_edge_note_rect_.y, style);
    }
}


// MapLayersPreviewWidget implementation moved from map_layers_preview_widget.cpp.
namespace {
constexpr double kTau = 6.28318530717958647692;

std::uint64_t generate_preview_seed() {
    static std::random_device rd;
    if (rd.entropy() > 0.0) {
        return (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(now.count());
}

SDL_Color hsv_to_rgb(float hue, float saturation, float value) {
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    saturation = std::clamp(saturation, 0.0f, 1.0f);
    value = std::clamp(value, 0.0f, 1.0f);

    const float chroma = value * saturation;
    const float h_prime = hue / 60.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(h_prime, 2.0f) - 1.0f));

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (0.0f <= h_prime && h_prime < 1.0f) {
        r = chroma;
        g = x;
    } else if (1.0f <= h_prime && h_prime < 2.0f) {
        r = x;
        g = chroma;
    } else if (2.0f <= h_prime && h_prime < 3.0f) {
        g = chroma;
        b = x;
    } else if (3.0f <= h_prime && h_prime < 4.0f) {
        g = x;
        b = chroma;
    } else if (4.0f <= h_prime && h_prime < 5.0f) {
        r = x;
        b = chroma;
    } else {
        r = chroma;
        b = x;
    }

    const float m = value - chroma;
    auto to_channel = [m](float c) {
        c = std::clamp(c + m, 0.0f, 1.0f);
        return static_cast<Uint8>(std::lround(c * 255.0f));
};
    return SDL_Color{to_channel(r), to_channel(g), to_channel(b), 255};
}

void draw_text(SDL_Renderer* renderer, const std::string& text, int x, int y, const DMLabelStyle& style) {
    if (!renderer || text.empty()) {
        return;
    }
    TTF_Font* font = style.open_font();
    if (!font) {
        return;
    }
    SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
    if (!surf) {
        TTF_CloseFont(font);
        return;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        sdl_render::Texture(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
    TTF_CloseFont(font);
}

void draw_circle(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color color, int thickness = 2) {
    if (!renderer || radius <= 0 || thickness <= 0) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int segments = std::max(32, radius * 4);
    const double step = kTau / static_cast<double>(segments);
    for (int layer = 0; layer < thickness; ++layer) {
        int r = std::max(1, radius - layer);
        int prev_x = cx + r;
        int prev_y = cy;
        for (int i = 1; i <= segments; ++i) {
            double angle = step * static_cast<double>(i);
            int x = cx + static_cast<int>(std::lround(std::cos(angle) * r));
            int y = cy + static_cast<int>(std::lround(std::sin(angle) * r));
            SDL_RenderLine(renderer, prev_x, prev_y, x, y);
            prev_x = x;
            prev_y = y;
        }
    }
}

void fill_circle(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color color) {
    if (!renderer || radius <= 0) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int y = -radius; y <= radius; ++y) {
        int dx = static_cast<int>(std::sqrt(static_cast<double>(radius * radius - y * y)));
        SDL_RenderLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
    }
}

void fill_ring(SDL_Renderer* renderer, int cx, int cy, int inner_radius, int outer_radius, SDL_Color color) {
    if (!renderer || outer_radius <= 0) {
        return;
    }
    inner_radius = std::max(0, std::min(inner_radius, outer_radius));
    if (inner_radius >= outer_radius) {
        fill_circle(renderer, cx, cy, outer_radius, color);
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int y = -outer_radius; y <= outer_radius; ++y) {
        int outer_dx = static_cast<int>(std::sqrt(static_cast<double>(outer_radius * outer_radius - y * y)));
        if (inner_radius == 0 || std::abs(y) > inner_radius) {
            SDL_RenderLine(renderer, cx - outer_dx, cy + y, cx + outer_dx, cy + y);
            continue;
        }
        int inner_dx = static_cast<int>(std::sqrt(static_cast<double>(inner_radius * inner_radius - y * y)));
        SDL_RenderLine(renderer, cx - outer_dx, cy + y, cx - inner_dx, cy + y);
        SDL_RenderLine(renderer, cx + inner_dx, cy + y, cx + outer_dx, cy + y);
    }
}
}

MapLayersPreviewWidget::MapLayersPreviewWidget()
    : preview_seed_(generate_preview_seed()) {}

MapLayersPreviewWidget::~MapLayersPreviewWidget() { remove_listener(); }

void MapLayersPreviewWidget::set_map_info(nlohmann::json* map_info) {
    map_info_ = map_info;
    mark_dirty();
}

void MapLayersPreviewWidget::set_controller(std::shared_ptr<MapLayersController> controller) {
    if (controller_ == controller) {
        return;
    }
    remove_listener();
    controller_ = std::move(controller);
    ensure_listener();
    mark_dirty();
}

void MapLayersPreviewWidget::set_on_select_layer(SelectLayerCallback cb) {
    on_select_layer_ = std::move(cb);
}

void MapLayersPreviewWidget::set_on_select_room(SelectRoomCallback cb) {
    on_select_room_ = std::move(cb);
}

void MapLayersPreviewWidget::set_on_show_room_list(ShowRoomListCallback cb) {
    on_show_room_list_ = std::move(cb);
}

void MapLayersPreviewWidget::set_on_change(std::function<void()> cb) {
    on_change_ = std::move(cb);
}

void MapLayersPreviewWidget::set_selected_layer(int index) {
    if (selected_layer_index_ == index) {
        return;
    }
    selected_layer_index_ = index;

    request_geometry_update();
}

void MapLayersPreviewWidget::set_layer_diagnostics(const std::vector<int>& invalid_layers,
                                                   const std::vector<int>& warning_layers,
                                                   const std::vector<int>& dependency_layers) {
    auto to_set = [](const std::vector<int>& values, std::unordered_set<int>& target) {
        target.clear();
        target.insert(values.begin(), values.end());
};
    to_set(invalid_layers, invalid_layers_);
    to_set(warning_layers, warning_layers_);
    to_set(dependency_layers, dependency_layers_);
    mark_dirty();
}

void MapLayersPreviewWidget::set_rect(const SDL_Rect& r) {
    rect_ = r;
    preview_rect_ = rect_;
    legend_rect_ = SDL_Rect{r.x, r.y, 0, r.h};

    const int gap = DMSpacing::panel_padding();
    const int min_preview_width = 240;
    int legend_width = 0;
    if (rect_.w > min_preview_width + gap + 120) {
        const int desired = std::clamp(rect_.w / 3, 160, 280);
        legend_width = std::min(desired, rect_.w - min_preview_width - gap);
        legend_width = std::max(0, legend_width);
    }

    const int spacing = (legend_width > 0) ? gap : 0;
    legend_rect_.x = rect_.x;
    legend_rect_.y = rect_.y;
    legend_rect_.w = legend_width;
    legend_rect_.h = rect_.h;

    preview_rect_.x = rect_.x + legend_width + spacing;
    preview_rect_.y = rect_.y;
    preview_rect_.w = std::max(0, rect_.w - legend_width - spacing);
    preview_rect_.h = rect_.h;

    preview_center_ = SDL_Point{preview_rect_.x + preview_rect_.w / 2, preview_rect_.y + preview_rect_.h / 2};

    const int button_margin = DMSpacing::panel_padding();
    const int raw_button_size = preview_rect_.w > 0 ? preview_rect_.w / 7 : 0;
    int button_size = std::clamp(raw_button_size, 26, 40);
    const int max_button_width = std::max(0, preview_rect_.w - button_margin * 2);
    const int max_button_height = std::max(0, preview_rect_.h - button_margin * 2);
    if (max_button_width > 0) {
        button_size = std::min(button_size, max_button_width);
    }
    if (max_button_height > 0) {
        button_size = std::min(button_size, max_button_height);
    }
    if (button_size > 0 && preview_rect_.w > 0 && preview_rect_.h > 0) {
        refresh_button_rect_.w = button_size;
        refresh_button_rect_.h = button_size;
        refresh_button_rect_.x = preview_rect_.x + button_margin;
        refresh_button_rect_.y = preview_rect_.y + preview_rect_.h - button_margin - button_size;
        refresh_button_rect_.y = std::max(refresh_button_rect_.y, preview_rect_.y + button_margin);
    } else {
        refresh_button_rect_ = SDL_Rect{0, 0, 0, 0};
    }
    refresh_hovered_ = false;
    recalculate_preview_scale();
}

int MapLayersPreviewWidget::height_for_width(int w) const {
    const int min_h = 280;
    const int max_h = 480;
    if (w <= min_h) {
        return min_h;
    }
    if (w >= max_h) {
        return max_h;
    }
    return w;
}

bool MapLayersPreviewWidget::handle_event(const SDL_Event& e) {
    ensure_latest_visuals();
    const bool pointer_event = (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP);
    if (!pointer_event) {
        return false;
    }
    SDL_Point p{0, 0};
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        p.x = e.motion.x;
        p.y = e.motion.y;
    } else {
        p.x = e.button.x;
        p.y = e.button.y;
    }
    const bool inside = SDL_PointInRect(&p, &rect_);
    if (!inside) {
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            if (refresh_hovered_) {
                refresh_hovered_ = false;
                request_geometry_update();
            }
            clear_hover_state();
        }
        return false;
    }

    const bool over_refresh = SDL_PointInRect(&p, &refresh_button_rect_);
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        if (refresh_hovered_ != over_refresh) {
            refresh_hovered_ = over_refresh;
            request_geometry_update();
        }
    }

    if (over_refresh) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            regenerate_preview();
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            clear_hover_state();
            return true;
        }
    }

    const int layer_hit = hit_test_layer(p.x, p.y);
    const std::string room_hit = hit_test_room(p.x, p.y);
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        update_hover_state(layer_hit, room_hit);
        return (layer_hit >= 0 || !room_hit.empty());
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        handle_preview_click(layer_hit, room_hit);
        return true;
    }
    return false;
}

void MapLayersPreviewWidget::render(SDL_Renderer* renderer) const {
    ensure_latest_visuals();
    render_preview(renderer);
}

void MapLayersPreviewWidget::mark_dirty() {
    dirty_ = true;
    preview_seed_ = generate_preview_seed();
    request_geometry_update();
}

void MapLayersPreviewWidget::create_new_room_entry() {
    if (!map_info_ || !map_info_->is_object()) {
        return;
    }
    nlohmann::json& rooms = (*map_info_)["rooms_data"];
    if (!rooms.is_object()) {
        rooms = nlohmann::json::object();
    }
    std::string base = "NewRoom";
    std::string key = base;
    int suffix = 1;
    while (rooms.contains(key)) {
        key = base + std::to_string(suffix++);
    }
    std::vector<SDL_Color> colors = utils::display_color::collect(rooms);
    nlohmann::json& entry = rooms[key];
    entry = nlohmann::json{{"name", key}};
    utils::display_color::ensure(entry, colors);
    mark_dirty();
    if (on_change_) {
        on_change_();
    }
}

void MapLayersPreviewWidget::regenerate_preview() {
    preview_seed_ = generate_preview_seed();
    dirty_ = true;
    request_geometry_update();
}

void MapLayersPreviewWidget::rebuild_visuals() {
    dirty_ = false;
    layer_visuals_.clear();
    room_legend_entries_.clear();
    max_visual_radius_ = 1.0;

    if (!map_info_) {
        preview_scale_ = 1.0;
        return;
    }

    const nlohmann::json& layers = layers_array();
    const nlohmann::json* rooms_info = rooms_data();
    if (!layers.is_array() || layers.empty()) {
        preview_scale_ = 1.0;
        return;
    }

    double min_edge_distance = map_layers::kDefaultMinEdgeDistance;
    if (map_info_ && map_info_->is_object()) {
        min_edge_distance = map_layers::min_edge_distance_from_map_manifest(*map_info_);
    }
    const map_layers::LayerRadiiResult radii = map_layers::compute_layer_radii(layers, rooms_info, min_edge_distance);
    min_edge_distance_ = radii.min_edge_distance;
    max_visual_radius_ = std::max(1.0, radii.map_radius);

    std::mt19937_64 rng(preview_seed_);

    layer_visuals_.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& layer_json = layers[i];
        if (!layer_json.is_object()) {
            continue;
        }
        LayerVisual visual;
        visual.index = static_cast<int>(i);
        visual.name = layer_json.value("name", std::string("Layer ") + std::to_string(i + 1));
        if (i < radii.layer_radii.size()) {
            visual.radius = radii.layer_radii[i];
        }
        if (i < radii.layer_extents.size()) {
            visual.extent = radii.layer_extents[i];
        }
        if (visual.index == 0) {
            visual.inner_radius = 0.0;
        } else {
            visual.inner_radius = std::max(0.0, visual.radius - visual.extent);
        }
        visual.color = layer_color(visual.index);
        visual.min_rooms = layer_json.value("min_rooms", 0);
        visual.max_rooms = layer_json.value("max_rooms", 0);
        if (visual.max_rooms > 0 && visual.max_rooms < visual.min_rooms) {
            visual.max_rooms = visual.min_rooms;
        }
        visual.invalid = invalid_layers_.find(visual.index) != invalid_layers_.end();
        visual.warning = warning_layers_.find(visual.index) != warning_layers_.end();
        visual.dependency = dependency_layers_.find(visual.index) != dependency_layers_.end();
        visual.selected = (visual.index == selected_layer_index_);

        const auto rooms_it = layer_json.find("rooms");
        if (rooms_it != layer_json.end() && rooms_it->is_array()) {

            for (const auto& candidate : *rooms_it) {
                if (!candidate.is_object()) {
                    continue;
                }
                const std::string name = candidate.value("name", std::string());
                if (name.empty()) {
                    continue;
                }
                RoomVisual room;
                room.layer_index = visual.index;
                room.key = name;
                room.display_name = display_name_for_room(room.key);
                room.color = room_color(room.key);
                room.extent = map_layers::room_extent_from_rooms_data(rooms_info, room.key);
                if (!(room.extent > 0.0)) {
                    room.extent = 1.0;
                }
                visual.rooms.push_back(std::move(room));
            }
        }
        if (visual.index == 0) {
            if (!visual.rooms.empty()) {
                for (auto& room : visual.rooms) {
                    room.angle = 0.0;
                    room.radius = 0.0;
                    room.position = {0.0f, 0.0f};
                }
            }
        } else if (!visual.rooms.empty()) {
            std::vector<double> extents;
            extents.reserve(visual.rooms.size());
            for (const auto& room : visual.rooms) {
                extents.push_back(room.extent > 0.0 ? room.extent : 1.0);
            }
            std::uniform_real_distribution<double> start_angle_dist(0.0, kTau);
            const double start_angle = start_angle_dist(rng);
            map_layers::RadialLayout layout = map_layers::compute_radial_layout(visual.radius, extents, min_edge_distance_, start_angle);
            if (!layout.angles.empty() && layout.angles.size() == visual.rooms.size()) {
                visual.radius = layout.radius;
                for (std::size_t idx = 0; idx < visual.rooms.size(); ++idx) {
                    const double raw_angle = layout.angles[idx];
                    const double normalized = std::fmod(raw_angle, kTau);
                    visual.rooms[idx].angle = (normalized < 0.0) ? (normalized + kTau) : normalized;
                    visual.rooms[idx].radius = layout.radius;
                    visual.rooms[idx].position.x = static_cast<float>(std::cos(raw_angle) * layout.radius);
                    visual.rooms[idx].position.y = static_cast<float>(std::sin(raw_angle) * layout.radius);
                }
            } else {
                const double step = kTau / static_cast<double>(visual.rooms.size());
                for (std::size_t idx = 0; idx < visual.rooms.size(); ++idx) {
                    const double angle = step * static_cast<double>(idx);
                    visual.rooms[idx].angle = angle;
                    visual.rooms[idx].radius = visual.radius;
                    visual.rooms[idx].position.x = static_cast<float>(std::cos(angle) * visual.radius);
                    visual.rooms[idx].position.y = static_cast<float>(std::sin(angle) * visual.radius);
                }
            }
        }
        if (visual.index == 0) {
            visual.inner_radius = 0.0;
        } else {
            visual.inner_radius = std::max(0.0, visual.radius - visual.extent);
        }
        visual.room_count = static_cast<int>(visual.rooms.size());
        layer_visuals_.push_back(std::move(visual));
        max_visual_radius_ = std::max(max_visual_radius_, layer_visuals_.back().radius + layer_visuals_.back().extent);
    }

    std::unordered_map<std::string, std::string> unique_rooms;
    for (const auto& layer : layer_visuals_) {
        for (const auto& room : layer.rooms) {
            if (room.key.empty()) {
                continue;
            }
            unique_rooms.emplace(room.key, room.display_name);
        }
    }

    room_legend_entries_.reserve(unique_rooms.size());
    for (const auto& [key, display] : unique_rooms) {
        RoomLegendEntry entry;
        entry.key = key;
        entry.display_name = display.empty() ? key : display;
        entry.color = room_color(key);
        room_legend_entries_.push_back(std::move(entry));
    }
    std::sort(room_legend_entries_.begin(), room_legend_entries_.end(), [](const RoomLegendEntry& a, const RoomLegendEntry& b) {
        return a.display_name < b.display_name;
    });
    recalculate_preview_scale();
}

void MapLayersPreviewWidget::ensure_latest_visuals() const {
    if (!dirty_) {
        return;
    }
    const_cast<MapLayersPreviewWidget*>(this)->rebuild_visuals();
}

void MapLayersPreviewWidget::recalculate_preview_scale() {
    preview_scale_ = compute_preview_scale();
}

double MapLayersPreviewWidget::compute_preview_scale() const {
    if (preview_rect_.w <= 0 || preview_rect_.h <= 0 || max_visual_radius_ <= 0.0) {
        return 1.0;
    }
    const int padding = DMSpacing::panel_padding();
    int usable = std::max(1, std::min(preview_rect_.w, preview_rect_.h) / 2 - padding);
    if (usable <= 0) {
        usable = 1;
    }
    return static_cast<double>(usable) / std::max(1.0, max_visual_radius_);
}

SDL_Color MapLayersPreviewWidget::layer_color(int index) const {
    if (index < 0) {
        index = 0;
    }
    const float golden_ratio = 0.61803398875f;
    const float hue = std::fmod((static_cast<float>(index) * golden_ratio) * 360.0f, 360.0f);
    return hsv_to_rgb(hue, 0.55f, 0.88f);
}

SDL_Color MapLayersPreviewWidget::room_color(const std::string& key) const {
    if (key.empty()) {
        return SDL_Color{200, 200, 200, 255};
    }
    const nlohmann::json* rooms_info = rooms_data();
    if (rooms_info && rooms_info->is_object()) {
        auto it = rooms_info->find(key);
        if (it != rooms_info->end() && it->is_object()) {
            auto color_it = it->find("display_color");
            if (color_it != it->end()) {
                if (auto parsed = utils::color::color_from_json(*color_it)) {
                    SDL_Color color = *parsed;
                    color.a = 255;
                    return color;
                }
            }
        }
    }
    std::size_t hash = std::hash<std::string>{}(key);
    const float golden_ratio = 0.61803398875f;
    float hue = std::fmod(static_cast<float>(hash % 360) + static_cast<float>(hash) * golden_ratio, 360.0f);
    float saturation = 0.6f + static_cast<float>((hash >> 8) % 40) / 100.0f;
    saturation = std::clamp(saturation, 0.55f, 0.95f);
    float value = 0.78f + static_cast<float>((hash >> 4) % 20) / 100.0f;
    value = std::clamp(value, 0.75f, 0.98f);
    return hsv_to_rgb(hue, saturation, value);
}

std::string MapLayersPreviewWidget::display_name_for_room(const std::string& key) const {
    const nlohmann::json* rooms_info = rooms_data();
    if (!rooms_info || !rooms_info->is_object()) {
        return key;
    }
    auto it = rooms_info->find(key);
    if (it == rooms_info->end() || !it->is_object()) {
        return key;
    }
    return it->value("name", key);
}

const nlohmann::json& MapLayersPreviewWidget::layers_array() const {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    if (!map_info_ || !map_info_->is_object()) {
        return kEmpty;
    }
    auto it = map_info_->find("map_layers");
    if (it == map_info_->end() || !it->is_array()) {
        return kEmpty;
    }
    return *it;
}

const nlohmann::json* MapLayersPreviewWidget::rooms_data() const {
    if (!map_info_ || !map_info_->is_object()) {
        return nullptr;
    }
    auto it = map_info_->find("rooms_data");
    if (it == map_info_->end() || !it->is_object()) {
        return nullptr;
    }
    return &(*it);
}

void MapLayersPreviewWidget::update_hover_state(int layer_index, const std::string& room_key) {
    bool changed = false;
    if (hovered_layer_index_ != layer_index) {
        hovered_layer_index_ = layer_index;
        changed = true;
    }
    if (hovered_room_key_ != room_key) {
        hovered_room_key_ = room_key;
        changed = true;
    }
    if (changed) {
        request_geometry_update();
    }
}

void MapLayersPreviewWidget::clear_hover_state() {
    update_hover_state(-1, std::string());
}

void MapLayersPreviewWidget::handle_preview_click(int layer_index, const std::string& room_key) {
    if (!room_key.empty()) {
        if (on_select_room_) {
            on_select_room_(room_key);
        }
        return;
    }
    if (layer_index >= 0) {
        if (on_select_layer_) {
            on_select_layer_(layer_index);
        }
        return;
    }
    if (on_show_room_list_) {
        on_show_room_list_();
    }
}

int MapLayersPreviewWidget::hit_test_layer(int x, int y) const {
    if (layer_visuals_.empty() || preview_rect_.w <= 0) {
        return -1;
    }
    SDL_Point point{x, y};
    if (!SDL_PointInRect(&point, &preview_rect_)) {
        return -1;
    }
    double scale = preview_scale_;
    if (scale <= 0.0) {
        scale = compute_preview_scale();
    }
    if (scale <= 0.0) {
        return -1;
    }
    const double dx = static_cast<double>(x - preview_center_.x);
    const double dy = static_cast<double>(y - preview_center_.y);
    const double dist_pixels = std::sqrt(dx * dx + dy * dy);
    const double tolerance = 6.0;
    for (const auto& layer : layer_visuals_) {
        if (layer.index == 0) {
            const double dot_radius = std::clamp(layer.extent * scale, 4.0, 18.0);
            if (dist_pixels <= dot_radius + tolerance) {
                return layer.index;
            }
            continue;
        }
        const double outer_pixels = layer.radius * scale;
        const double inner_pixels = layer.inner_radius * scale;
        const double min_radius = std::max(0.0, inner_pixels - tolerance);
        const double max_radius = std::max(outer_pixels, inner_pixels) + tolerance;
        if (outer_pixels <= 0.0 || max_radius <= 0.0) {
            continue;
        }
        if (dist_pixels >= min_radius && dist_pixels <= max_radius) {
            return layer.index;
        }
    }
    return -1;
}

std::string MapLayersPreviewWidget::hit_test_room(int x, int y) const {
    if (layer_visuals_.empty() || preview_rect_.w <= 0) {
        return {};
    }
    SDL_Point point{x, y};
    if (!SDL_PointInRect(&point, &preview_rect_)) {
        return {};
    }
    double scale = preview_scale_;
    if (scale <= 0.0) {
        scale = compute_preview_scale();
    }
    if (scale <= 0.0) {
        return {};
    }
    const double px = static_cast<double>(x);
    const double py = static_cast<double>(y);
    const double base_radius = 12.0;
    for (const auto& layer : layer_visuals_) {
        for (const auto& room : layer.rooms) {
            const double rx = preview_center_.x + static_cast<double>(room.position.x) * scale;
            const double ry = preview_center_.y + static_cast<double>(room.position.y) * scale;
            const double dx = px - rx;
            const double dy = py - ry;
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double room_radius = std::max(base_radius, room.extent * scale * 0.6);
            if (dist <= room_radius) {
                return room.key;
            }
        }
    }
    return {};
}

void MapLayersPreviewWidget::ensure_listener() {
    if (!controller_ || controller_listener_id_ != 0) {
        return;
    }
    controller_listener_id_ = controller_->add_listener([this]() { this->mark_dirty(); });
}

void MapLayersPreviewWidget::remove_listener() {
    if (controller_ && controller_listener_id_ != 0) {
        controller_->remove_listener(controller_listener_id_);
    }
    controller_listener_id_ = 0;
}

void MapLayersPreviewWidget::render_preview(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    SDL_Rect rect = preview_rect_;
    if (rect.w <= 0 || rect.h <= 0) {
        render_room_legend(renderer);
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color bg = DMStyles::PanelBG();
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    sdl_render::FillRect(renderer, &rect);

    const SDL_Color border = DMStyles::Border();
    dm_draw::DrawRoundedOutline(renderer, rect, DMStyles::CornerRadius(), 1, border);

    if (layer_visuals_.empty() || max_visual_radius_ <= 0.0) {
        draw_text(renderer, "No layers configured.", rect.x + 16, rect.y + 16, DMStyles::Label());
        render_refresh_button(renderer);
        render_room_legend(renderer);
        return;
    }

    const SDL_Point center = preview_center_;
    const int hovered_layer = hovered_layer_index_;
    const std::string hovered_room = hovered_room_key_;
    const_cast<MapLayersPreviewWidget*>(this)->preview_scale_ = compute_preview_scale();

    const SDL_Color invalid_color{214, 63, 87, 255};
    const SDL_Color warning_color{234, 179, 8, 255};
    const SDL_Color dependency_color{125, 200, 255, 255};
    const SDL_Color selection_outline = DMStyles::AccentButton().border;

    const DMLabelStyle base_label = DMStyles::Label();
    const int label_line_height = base_label.font_size + DMSpacing::small_gap();

    for (const auto& layer : layer_visuals_) {
        SDL_Color outline_color = layer.color;
        if (layer.invalid) {
            outline_color = invalid_color;
        } else if (layer.warning) {
            outline_color = warning_color;
        } else if (layer.dependency) {
            outline_color = lighten(outline_color, 0.2f);
        }
        const bool hovered_layer_active = (hovered_layer == layer.index && hovered_room.empty());
        const bool selected_layer = layer.selected;

        if (layer.index == 0) {
            const double raw_dot = std::max(layer.extent, 1.0) * preview_scale_;
            const int dot_radius = std::clamp(static_cast<int>(std::lround(raw_dot)), 4, 18);
            SDL_Color fill_color = lighten(outline_color, selected_layer ? 0.25f : 0.1f);
            fill_color.a = selected_layer ? 180 : 140;
            if (hovered_layer_active && !selected_layer) {
                fill_color = lighten(fill_color, 0.2f);
            }
            fill_circle(renderer, center.x, center.y, dot_radius, fill_color);
            SDL_Color border_color = outline_color;
            if (hovered_layer_active) {
                border_color = lighten(border_color, 0.25f);
            }
            int thickness = selected_layer ? 4 : 3;
            draw_circle(renderer, center.x, center.y, dot_radius, border_color, thickness);
            if (selected_layer) {
                draw_circle(renderer, center.x, center.y, dot_radius + 3, selection_outline, 1);
            }
        } else {
            const int radius_pixels = std::max(1, static_cast<int>(std::lround(layer.radius * preview_scale_)));
            const int inner_radius_pixels = std::max(0, static_cast<int>(std::lround(layer.inner_radius * preview_scale_)));

            if (hovered_layer_active || selected_layer) {
                SDL_Color ring_color = lighten(outline_color, selected_layer ? 0.12f : 0.25f);
                ring_color.a = selected_layer ? 140 : 100;
                fill_ring(renderer, center.x, center.y, inner_radius_pixels, radius_pixels, ring_color);
            }

            SDL_Color color = outline_color;
            int thickness = selected_layer ? 6 : 3;
            if (hovered_layer_active) {
                color = lighten(color, 0.25f);
                thickness = std::max(thickness, selected_layer ? 7 : 5);
            }
            draw_circle(renderer, center.x, center.y, radius_pixels, color, thickness);
            if (selected_layer) {
                draw_circle(renderer, center.x, center.y, radius_pixels + 4, selection_outline, 1);
            }
        }

        std::ostringstream oss;
        oss << layer.name;
        oss << " • " << layer.room_count << (layer.room_count == 1 ? " room" : " rooms");
        oss << " • " << layer.min_rooms << "-" << layer.max_rooms << " total";
        if (layer.invalid) {
            oss << " • fix issues";
        } else if (layer.warning) {
            oss << " • review";
        }
        DMLabelStyle label_style = base_label;
        if (layer.invalid) {
            label_style.color = invalid_color;
        } else if (layer.warning) {
            label_style.color = warning_color;
        } else if (layer.selected) {
            label_style.color = lighten(label_style.color, 0.1f);
        }
        int text_x = rect.x + DMSpacing::small_gap();
        int text_y = rect.y + DMSpacing::small_gap() + layer.index * label_line_height;
        draw_text(renderer, oss.str(), text_x, text_y, label_style);
    }

    for (const auto& layer : layer_visuals_) {
        if (layer.index == 0) {
            continue;
        }
        for (const auto& room : layer.rooms) {
            const int px = center.x + static_cast<int>(std::lround(room.position.x * preview_scale_));
            const int py = center.y + static_cast<int>(std::lround(room.position.y * preview_scale_));
            const double extent_pixels = std::max(8.0, room.extent * preview_scale_ * 0.75);
            const int radius_pixels = static_cast<int>(std::round(extent_pixels));
            SDL_Color base_fill = room.color;
            SDL_Color outline = darken(base_fill, 0.2f);
            if (layer.invalid) {
                outline = invalid_color;
            } else if (layer.warning) {
                outline = warning_color;
            } else if (layer.dependency) {
                outline = dependency_color;
            } else if (layer.selected) {
                outline = lighten(outline, 0.15f);
            }
            SDL_Color fill = base_fill;
            if (layer.selected) {
                fill = lighten(fill, 0.12f);
            }
            if (!hovered_room.empty() && hovered_room == room.key) {
                fill = lighten(fill, 0.18f);
            }
            fill.a = (hovered_room == room.key) ? 200 : 160;
            fill_circle(renderer, px, py, radius_pixels, fill);
            draw_circle(renderer, px, py, radius_pixels, outline, (hovered_room == room.key) ? 3 : 2);
        }
    }

    const int footer_gap = DMSpacing::small_gap();
    const int footer_radius_y = rect.y + rect.h - (base_label.font_size + footer_gap * 3);
    int footer_text_x = rect.x + footer_gap;
    if (refresh_button_rect_.w > 0) {
        footer_text_x = std::max(footer_text_x, refresh_button_rect_.x + refresh_button_rect_.w + footer_gap);
    }

    std::ostringstream radius_stream;
    radius_stream << "Map radius ≈ " << std::fixed << std::setprecision(0) << max_visual_radius_;
    draw_text(renderer, radius_stream.str(), footer_text_x, footer_radius_y, base_label);

    DMLabelStyle footer_label = DMStyles::Label();
    const int footer_info_y = rect.y + rect.h - (footer_label.font_size + footer_gap * 2);
    if (!hovered_room.empty()) {
        std::string label = display_name_for_room(hovered_room);
        if (label.empty()) {
            label = hovered_room;
        }
        draw_text(renderer, label, footer_text_x, footer_info_y, footer_label);
    } else if (hovered_layer >= 0) {
        auto it = std::find_if(layer_visuals_.begin(), layer_visuals_.end(), [&](const LayerVisual& v) {
            return v.index == hovered_layer;
        });
        if (it != layer_visuals_.end()) {
            std::ostringstream oss;
            oss << it->name << " • " << it->room_count << (it->room_count == 1 ? " room" : " rooms");
            oss << " • " << it->min_rooms << "-" << it->max_rooms << " total";
            draw_text(renderer, oss.str(), footer_text_x, footer_info_y, footer_label);
        }
    }

    render_refresh_button(renderer);
    render_room_legend(renderer);
}

void MapLayersPreviewWidget::render_refresh_button(SDL_Renderer* renderer) const {
    if (!renderer || refresh_button_rect_.w <= 0 || refresh_button_rect_.h <= 0) {
        return;
    }

    SDL_Rect button_rect = refresh_button_rect_;
    const DMButtonStyle& style = DMStyles::AccentButton();
    SDL_Color fill = refresh_hovered_ ? style.hover_bg : style.bg;
    const int corner_radius = std::max(4, DMStyles::CornerRadius() / 2);

    dm_draw::DrawBeveledRect(renderer, button_rect, corner_radius, DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), true, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, button_rect, corner_radius, 1, style.border);

    DMLabelStyle icon_style = style.label;
    icon_style.color = style.text;
    if (refresh_hovered_) {
        icon_style.color = lighten(icon_style.color, 0.08f);
    }

    const std::string refresh_icon = "\xE2\x86\xBB";
    int text_w = 0;
    int text_h = icon_style.font_size;
    if (TTF_Font* font = icon_style.open_font()) {
        if (!ttf_util::GetStringSize(font, refresh_icon, &text_w, &text_h)) {
            text_w = 0;
            text_h = icon_style.font_size;
        }
        TTF_CloseFont(font);
    }
    const int text_x = button_rect.x + (button_rect.w - text_w) / 2;
    const int text_y = button_rect.y + (button_rect.h - text_h) / 2;
    draw_text(renderer, refresh_icon, text_x, text_y, icon_style);
}

void MapLayersPreviewWidget::render_room_legend(SDL_Renderer* renderer) const {
    if (!renderer || legend_rect_.w <= 0 || legend_rect_.h <= 0) {
        return;
    }

    SDL_Rect legend = legend_rect_;
    const SDL_Color panel_bg = DMStyles::PanelBG();
    SDL_Color legend_bg = lighten(panel_bg, 0.06f);
    legend_bg.a = panel_bg.a;
    SDL_SetRenderDrawColor(renderer, legend_bg.r, legend_bg.g, legend_bg.b, legend_bg.a);
    sdl_render::FillRect(renderer, &legend);

    const SDL_Color border_color = DMStyles::Border();
    dm_draw::DrawRoundedOutline(renderer, legend, DMStyles::CornerRadius(), 1, border_color);

    const DMLabelStyle base_label = DMStyles::Label();
    DMLabelStyle header_style = base_label;
    header_style.color = lighten(header_style.color, 0.15f);

    const int padding = DMSpacing::small_gap();
    int text_x = legend_rect_.x + padding;
    int y = legend_rect_.y + padding;

    draw_text(renderer, "Room Key", text_x, y, header_style);
    y += header_style.font_size + padding;

    if (room_legend_entries_.empty()) {
        draw_text(renderer, "No rooms", text_x, y, base_label);
        return;
    }

    const int swatch_size = 18;
    for (const auto& entry : room_legend_entries_) {
        bool hovered = (hovered_room_key_ == entry.key);
        SDL_Rect swatch{text_x, y, swatch_size, swatch_size};
        SDL_Color fill = entry.color;
        if (hovered) {
            fill = lighten(fill, 0.15f);
        }
        fill.a = hovered ? 220 : 180;
        SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
        sdl_render::FillRect(renderer, &swatch);
        SDL_SetRenderDrawColor(renderer, border_color.r, border_color.g, border_color.b, border_color.a);
        sdl_render::Rect(renderer, &swatch);

        DMLabelStyle label_style = base_label;
        if (hovered) {
            label_style.color = lighten(label_style.color, 0.1f);
        }
        int label_x = swatch.x + swatch.w + padding;
        draw_text(renderer, entry.display_name, label_x, y, label_style);
        y += swatch_size + padding;
        if (y > legend_rect_.y + legend_rect_.h - swatch_size) {
            break;
        }
    }
}








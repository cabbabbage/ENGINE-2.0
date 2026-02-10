#include "ChildrenTimelinesPanel.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

#include <SDL3/SDL_log.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "assets/animation_child_data.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "core/AssetsManager.hpp"
#include "assets/asset/asset_info.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/widgets.hpp"
#include "devtools/asset_library_ui.hpp"
#include "utils/input.hpp"
#include "devtools/animation_runtime_refresh.hpp"

namespace animation_editor {
namespace {
constexpr int kDefaultPanelWidth = 360;
constexpr int kDefaultPanelHeight = 260;

const DMButtonStyle& enabled_button_style() {
    return DMStyles::AccentButton();
}

const DMButtonStyle& disabled_button_style() {
    return DMStyles::HeaderButton();
}

const DMButtonStyle& delete_button_style() {
    return DMStyles::DeleteButton();
}

class ChildLabelWidget : public Widget {
  public:
    explicit ChildLabelWidget(std::string text, bool full_row = false)
        : text_(std::move(text)), full_row_(full_row) {}

    void set_text(std::string text) { text_ = std::move(text); }

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return DMCheckbox::height(); }
    bool handle_event(const SDL_Event&) override { return false; }
    bool wants_full_row() const override { return full_row_; }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer) return;
        const auto& style = DMStyles::Label();
        TTF_Font* font = TTF_OpenFont(style.font_path.c_str(), style.font_size);
        if (!font) return;
        SDL_Surface* surface = ttf_util::RenderTextBlended(font, text_.c_str(), style.color);
        if (!surface) {
            TTF_CloseFont(font);
            return;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect dst{rect_.x, rect_.y + (rect_.h - surface->h) / 2, surface->w, surface->h};
            sdl_render::Texture(renderer, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_DestroySurface(surface);
        TTF_CloseFont(font);
    }

  private:
    std::string text_{};
    SDL_Rect rect_{0, 0, 0, DMCheckbox::height()};
    bool full_row_ = false;
};
}

ChildrenTimelinesPanel::ChildrenTimelinesPanel()
    : DockableCollapsible("Children & Timelines", true , kDefaultPanelWidth, kDefaultPanelHeight) {
    set_show_header(true);

    toggle_assets_button_ = std::make_unique<DMButton>("Find Asset", &enabled_button_style(), kDefaultPanelWidth, DMButton::height());
    toggle_assets_widget_ = std::make_unique<ButtonWidget>(toggle_assets_button_.get(), [this]() { this->open_asset_picker(); });
    children_header_widget_ = std::make_unique<ChildLabelWidget>("Children", true);

    set_expanded(false);  // collapsed by default
    rebuild_rows();
}

void ChildrenTimelinesPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    if (document_ == document) {
        return;
    }
    document_ = std::move(document);
    close_asset_picker();
    last_signature_.clear();
    sync_from_document();
}

void ChildrenTimelinesPanel::set_manifest_store(devmode::core::ManifestStore* manifest_store) {
    if (manifest_store_ == manifest_store) {
        return;
    }
    manifest_store_ = manifest_store;
    if (!manifest_store_) {
        close_asset_picker();
    }
    last_signature_.clear();
    sync_from_document();
}

void ChildrenTimelinesPanel::set_assets(Assets* assets) {
    if (assets_ == assets) {
        return;
    }
    assets_ = assets;
    close_asset_picker();
}

void ChildrenTimelinesPanel::set_status_callback(std::function<void(const std::string&, int)> callback) {
    status_callback_ = std::move(callback);
}

void ChildrenTimelinesPanel::set_on_children_changed(std::function<void(const std::vector<std::string>&)> callback) {
    on_children_changed_ = std::move(callback);
}

void ChildrenTimelinesPanel::refresh() {
    sync_from_document();
}

void ChildrenTimelinesPanel::update(const Input& input, int screen_w, int screen_h) {
    picker_screen_w_ = screen_w;
    picker_screen_h_ = screen_h;
    sync_from_document();
    if (asset_picker_ui_ && asset_picker_ui_->is_visible() && assets_ && manifest_store_) {
        sync_asset_picker_geometry();
        asset_picker_ui_->update(input, screen_w, screen_h, assets_->library(), *assets_, *manifest_store_);
    }
}

bool ChildrenTimelinesPanel::handle_event(const SDL_Event& e) {
    if (!is_visible()) {
        return false;
    }

    if (asset_picker_ui_ && asset_picker_ui_->is_visible()) {
        if (asset_picker_ui_->handle_event(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            close_asset_picker();
            return true;
        }
    }
    std::vector<bool> previous_async;
    previous_async.reserve(child_rows_.size());
    for (const auto& row : child_rows_) {
        previous_async.push_back(row.async_checkbox ? row.async_checkbox->value() : false);
    }

    bool consumed = DockableCollapsible::handle_event(e);

    for (std::size_t i = 0; i < child_rows_.size(); ++i) {
        const auto& row = child_rows_[i];
        const bool next = row.async_checkbox ? row.async_checkbox->value() : false;
        if (i < previous_async.size() && next != previous_async[i]) {
            apply_child_mode(row.name, next ? AnimationChildMode::Async : AnimationChildMode::Static);
            consumed = true;
        }
    }

    return consumed;
}

void ChildrenTimelinesPanel::render(SDL_Renderer* renderer) const {
    DockableCollapsible::render(renderer);
    if (asset_picker_ui_ && asset_picker_ui_->is_visible()) {
        asset_picker_ui_->render(renderer, picker_screen_w_, picker_screen_h_);
    }
}

void ChildrenTimelinesPanel::set_work_area_bounds(const SDL_Rect& bounds) {
    set_work_area(bounds);
}

void ChildrenTimelinesPanel::rebuild_rows() {
    Rows rows;
    if (toggle_assets_widget_) {
        rows.push_back({ toggle_assets_widget_.get() });
    }
    if (children_header_widget_) {
        rows.push_back({ children_header_widget_.get() });
    }

    for (auto& row : child_rows_) {
        Row child_row;
        if (row.label_widget) child_row.push_back(row.label_widget.get());
        if (row.async_widget) child_row.push_back(row.async_widget.get());
        if (row.delete_widget) child_row.push_back(row.delete_widget.get());
        rows.push_back(std::move(child_row));
    }

    set_rows(rows);
    // Keep current expanded state; caller controls.
}

void ChildrenTimelinesPanel::open_asset_picker() {
    if (!assets_ || !manifest_store_) {
        if (status_callback_) {
            status_callback_("Asset picker unavailable.", 180);
        }
        return;
    }

    if (!asset_picker_ui_) {
        asset_picker_ui_ = std::make_unique<AssetLibraryUI>();
    }

    AssetLibraryUI::PickerModeOptions options;
    options.enabled = true;
    options.title = "Find Child Asset";
    options.on_selected = [this](const std::shared_ptr<AssetInfo>& info) {
        if (!info) {
            return;
        }
        if (is_existing_child(info->name)) {
            if (status_callback_) status_callback_("Child already exists.", 180);
            return;
        }
        add_child(info->name);
    };
    asset_picker_ui_->set_picker_mode(std::move(options));

    sync_asset_picker_geometry();
    asset_picker_ui_->open();
}

void ChildrenTimelinesPanel::close_asset_picker() {
    if (asset_picker_ui_) {
        asset_picker_ui_->close();
        asset_picker_ui_->set_picker_mode({});
    }
}

void ChildrenTimelinesPanel::sync_asset_picker_geometry() {
    if (!asset_picker_ui_) {
        return;
    }
    const SDL_Rect panel_rect = rect();
    int picker_x = panel_rect.x + panel_rect.w + DMSpacing::panel_padding();
    int picker_y = panel_rect.y;
    const int min_margin = DMSpacing::panel_padding();
    if (picker_screen_w_ > 0) {
        const int max_x = std::max(min_margin, picker_screen_w_ - 420);
        picker_x = std::clamp(picker_x, min_margin, max_x);
    }
    if (picker_screen_h_ > 0) {
        const int max_y = std::max(min_margin, picker_screen_h_ - 260);
        picker_y = std::clamp(picker_y, min_margin, max_y);
    }
    asset_picker_ui_->set_position(picker_x, picker_y);
}


bool ChildrenTimelinesPanel::is_existing_child(const std::string& name) const {
    return std::any_of(child_rows_.begin(), child_rows_.end(), [&](const ChildRow& row) { return row.name == name; });
}

std::string ChildrenTimelinesPanel::document_asset_name() const {
    if (!document_) {
        return {};
    }
    std::filesystem::path root = document_->asset_root();
    if (!root.empty()) {
        return root.filename().string();
    }
    std::filesystem::path info = document_->info_path();
    if (!info.empty()) {
        return info.stem().string();
    }
    return {};
}

void ChildrenTimelinesPanel::refresh_runtime() {
    if (!assets_) {
        return;
    }
    const std::string asset_name = document_asset_name();
    if (asset_name.empty()) {
        return;
    }
    auto info = assets_->library().get(asset_name);
    if (!info) {
        return;
    }
    if (auto* renderer = assets_->renderer()) {
        info->reload_animations_from_disk();
        info->loadAnimations(renderer);
    }
    devmode::refresh_loaded_animation_instances(assets_, info);
    assets_->mark_active_assets_dirty();
}

void ChildrenTimelinesPanel::sync_from_document() {
    const std::string signature = current_signature();
    if (signature == last_signature_) {
        sync_child_rows();
        return;
    }
    last_signature_ = signature;
    child_rows_.clear();

    if (!document_) {
        children_header_widget_ = std::make_unique<ChildLabelWidget>("Children (0)", true);
        rebuild_rows();
        return;
    }

    const auto animation_ids = document_->animation_ids();
    const std::string animation_id = animation_ids.empty() ? std::string{} : animation_ids.front();
    const auto children = document_->animation_children();
    children_header_widget_ = std::make_unique<ChildLabelWidget>(
        std::string("Children (") + std::to_string(children.size()) + ")", true);

    for (const auto& child : children) {
        ChildRow row;
        row.name = child;
        row.label_widget = std::make_unique<ChildLabelWidget>(child);
        const AnimationChildMode mode = animation_id.empty() ? AnimationChildMode::Static : child_mode(animation_id, child);
        row.async_checkbox = std::make_unique<DMCheckbox>("Async", mode == AnimationChildMode::Async);
        row.async_widget = std::make_unique<CheckboxWidget>(row.async_checkbox.get());
        row.delete_button = std::make_unique<DMButton>("x", &delete_button_style(), 36, DMButton::height());
        row.delete_widget = std::make_unique<ButtonWidget>(row.delete_button.get(), [this, child]() { this->remove_child(child); });
        child_rows_.push_back(std::move(row));
    }

    rebuild_rows();
}

void ChildrenTimelinesPanel::sync_child_rows() {
    if (!document_) {
        return;
    }

    const auto animation_ids = document_->animation_ids();
    if (animation_ids.empty()) {
        return;
    }

    const std::string animation_id = animation_ids.front();
    for (auto& row : child_rows_) {
        const AnimationChildMode mode = child_mode(animation_id, row.name);
        if (row.async_checkbox) {
            row.async_checkbox->set_value(mode == AnimationChildMode::Async);
        }
    }
}

void ChildrenTimelinesPanel::add_child(const std::string& asset_name) {
    if (!document_) {
        return;
    }
    auto children = document_->animation_children();
    auto it = std::find(children.begin(), children.end(), asset_name);
    if (it != children.end()) {
        if (status_callback_) status_callback_("Child already exists.", 180);
        return;
    }
    children.push_back(asset_name);
    close_asset_picker();
    document_->replace_animation_children(children);
    try {
        document_->save_to_file();
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ChildrenTimelinesPanel] Failed to save animation document after adding child.");
    }
    refresh_runtime();
    if (on_children_changed_) {
        on_children_changed_(children);
    }
    last_signature_.clear();
    sync_from_document();
    if (status_callback_) {
        status_callback_(std::string("Added child '") + asset_name + "'.", 180);
    }
}

void ChildrenTimelinesPanel::remove_child(const std::string& child_name) {
    if (!document_) {
        return;
    }

    auto children = document_->animation_children();
    auto it = std::find(children.begin(), children.end(), child_name);
    if (it == children.end()) {
        if (status_callback_) status_callback_("Child not found.", 180);
        return;
    }

    children.erase(it);
    document_->replace_animation_children(children);
    try {
        document_->save_to_file();
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ChildrenTimelinesPanel] Failed to save animation document after removing child.");
    }
    refresh_runtime();
    if (on_children_changed_) {
        on_children_changed_(children);
    }
    last_signature_.clear();
    sync_from_document();
    if (status_callback_) {
        status_callback_(std::string("Removed child '") + child_name + "'.", 180);
    }
}

void ChildrenTimelinesPanel::apply_child_mode(const std::string& child_name, AnimationChildMode mode) {
    if (!document_) {
        return;
    }

    const bool changed = apply_mode_to_all_animations(child_name, mode);
    if (!changed) {
        return;
    }

    try {
        document_->save_to_file();
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[ChildrenTimelinesPanel] Failed to save animation document after child mode change.");
    }

    refresh_runtime();
    last_signature_.clear();
    sync_from_document();
}

std::string ChildrenTimelinesPanel::current_signature() const {
    if (!document_) {
        return std::string{};
    }
    std::string signature = document_->animation_children_signature();
    auto animations = document_->animation_ids();
    for (const auto& id : animations) {
        signature.append("|").append(id);
        if (auto payload = document_->animation_payload(id)) {
            signature.append(":").append(*payload);
        }
    }
    return signature;
}

AnimationChildMode ChildrenTimelinesPanel::child_mode(const std::string& animation_id, const std::string& child_name) const {
    auto settings = document_ ? document_->child_timeline_settings(animation_id, child_name) : AnimationDocument::ChildTimelineSettings{};
    if (!settings.found) {
        return AnimationChildMode::Static;
    }
    return settings.mode;
}

bool ChildrenTimelinesPanel::apply_mode_to_all_animations(const std::string& child_name, AnimationChildMode mode) {
    if (!document_) {
        return false;
    }
    const bool auto_start = (mode == AnimationChildMode::Static);
    return document_->set_child_mode_for_all_animations(child_name, mode, auto_start);
}

}


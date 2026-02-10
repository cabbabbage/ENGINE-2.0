#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/animation_child_data.hpp"
#include "devtools/DockableCollapsible.hpp"

class ButtonWidget;
class DMButton;
class DMTextBox;
class DMCheckbox;
class Assets;
class AssetLibraryUI;
class Input;

namespace devmode::core {
class ManifestStore;
}

namespace animation_editor {

class AnimationDocument;

class ChildrenTimelinesPanel : public DockableCollapsible {
  public:
    ChildrenTimelinesPanel();

    void set_document(std::shared_ptr<AnimationDocument> document);
    void set_manifest_store(devmode::core::ManifestStore* manifest_store);
    void set_status_callback(std::function<void(const std::string&, int)> callback);
    void set_on_children_changed(std::function<void(const std::vector<std::string>&)> callback);
    void set_assets(Assets* assets);

    void refresh();
    void update(const Input& input, int screen_w, int screen_h);

    bool handle_event(const SDL_Event& e) override;
    void render(SDL_Renderer* renderer) const override;

    void set_work_area_bounds(const SDL_Rect& bounds);

  private:
    void rebuild_rows();
    void sync_from_document();
    void sync_child_rows();
    void open_asset_picker();
    void close_asset_picker();
    void sync_asset_picker_geometry();
    bool is_existing_child(const std::string& name) const;
    void refresh_runtime();
    std::string document_asset_name() const;
    void add_child(const std::string& asset_name);
    void remove_child(const std::string& child_name);
    void apply_child_mode(const std::string& child_name, AnimationChildMode mode);
    std::string current_signature() const;
    AnimationChildMode child_mode(const std::string& animation_id, const std::string& child_name) const;
    bool apply_mode_to_all_animations(const std::string& child_name, AnimationChildMode mode);

  private:
    std::shared_ptr<AnimationDocument> document_;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    Assets* assets_ = nullptr;
    std::function<void(const std::string&, int)> status_callback_;
    std::function<void(const std::vector<std::string>&)> on_children_changed_;

    struct ChildRow {
        std::string name;
        std::unique_ptr<Widget> label_widget;
        std::unique_ptr<DMCheckbox> async_checkbox;
        std::unique_ptr<Widget> async_widget;
        std::unique_ptr<DMButton> delete_button;
        std::unique_ptr<Widget> delete_widget;
};

    std::vector<ChildRow> child_rows_;
    std::unique_ptr<Widget> children_header_widget_;
    std::unique_ptr<AssetLibraryUI> asset_picker_ui_;
    int picker_screen_w_ = 0;
    int picker_screen_h_ = 0;
    std::unique_ptr<DMButton> toggle_assets_button_;
    std::unique_ptr<ButtonWidget> toggle_assets_widget_;

    std::string last_signature_;
};

}


#pragma once

#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>
#include <unordered_map>

#include "DockableCollapsible.hpp"

class DMTextBox;
class TextBoxWidget;
class Widget;
class Assets;
class AssetInfo;

class AssetListView {
public:
    struct Entry {
        std::string label;
        std::string value;
        bool is_tag = false;
        std::string manifest_name;
        std::vector<std::string> tags;
        std::shared_ptr<AssetInfo> info;
        bool recommended = false;
    };

    struct Callbacks {
        std::function<void(const Entry&)> on_click;
        std::function<void(const Entry&)> on_right_click;
        std::function<void(const Entry&)> on_delete;
        std::function<void(const Entry&, bool)> on_multi_select_toggle;
    };

    AssetListView();
    ~AssetListView();

    DMTextBox* search_box() const;
    TextBoxWidget* search_widget() const;

    void set_assets(Assets* assets);
    void set_entries(std::vector<Entry> entries);
    void set_callbacks(Callbacks callbacks);
    void set_multi_select_enabled(bool enabled);
    void set_selected_values(std::unordered_set<std::string> selected);
    void set_query(const std::string& query);
    const std::string& query() const;
    bool update_query_from_widget();

    void refresh_tiles();

    std::vector<Widget*> tile_ptrs() const;
    DockableCollapsible::Rows rows(std::size_t per_row = 2, bool include_search_row = true) const;
    void append_rows(DockableCollapsible::Rows& rows, std::size_t per_row = 2, bool include_search_row = true) const;

    SDL_Texture* preview_texture_for(const Entry& entry) const;

    static bool matches_query(const Entry& entry, const std::string& query);

private:
    class AssetTileWidget;
    SDL_Texture* default_frame_texture(const AssetInfo& info) const;
    void rebuild_tiles();

    std::unique_ptr<DMTextBox> search_box_;
    std::unique_ptr<TextBoxWidget> search_widget_;
    std::vector<Entry> entries_;
    std::vector<std::unique_ptr<AssetTileWidget>> tiles_;
    Callbacks callbacks_{};
    bool multi_select_enabled_ = false;
    std::unordered_set<std::string> selected_values_;
    std::string query_;
    Assets* assets_ = nullptr;
    mutable std::unordered_map<std::string, SDL_Texture*> preview_cache_;
};

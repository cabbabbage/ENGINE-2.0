#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include <unordered_map>

#include "DockManager.hpp"
#include "asset_list_view.hpp"

namespace devmode::core {
class ManifestStore;
}

class DockableCollapsible;
class Input;
class Assets;
class AssetInfo;

class SearchAssets {
public:
    struct Result {
        std::string label;
        std::string value;
        bool is_tag = false;
        std::string manifest_name;
        std::vector<std::string> tags;
        bool recommended = false;
};

    using ExtraResultsProvider = std::function<std::vector<Result>()>;
    using AssetFilter = std::function<bool(const nlohmann::json&)>;

    using Callback = std::function<void(const std::string&)>;
    using MultiSelectCallback = std::function<void(const std::vector<std::string>&)>;
    explicit SearchAssets(devmode::core::ManifestStore* manifest_store = nullptr);
    ~SearchAssets();
    void set_position(int x, int y);
    void set_screen_dimensions(int width, int height);
    void set_floating_stack_key(std::string key);
    void set_anchor_position(int x, int y);
    void layout_with_parent(const DockManager::SlidingParentInfo& parent);
    void open(Callback cb);
    void open_multi_select(MultiSelectCallback cb);
    void close();
    bool visible() const;
    void set_embedded_mode(bool embedded);
    void set_embedded_rect(const SDL_Rect& rect);
    SDL_Rect rect() const;
    void update(const Input& input);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* r) const;
    bool is_point_inside(int x, int y) const;
    void set_manifest_store(devmode::core::ManifestStore* manifest_store);
    void set_assets(Assets* assets);
    void set_query_for_testing(const std::string& value);
    std::vector<std::pair<std::string, bool>> results_for_testing() const;
    void set_extra_results_provider(ExtraResultsProvider provider);
    void set_asset_filter(AssetFilter filter);
private:
    std::size_t columns_per_row() const;
    struct Asset {
        std::string name;
        std::string manifest_name;
        std::vector<std::string> tags;
        std::shared_ptr<AssetInfo> info;
        const nlohmann::json* payload = nullptr;
    };
    void load_assets();
    void filter_assets();
    void activate_result(const Result& result);
    static std::string to_lower(std::string s);
    void apply_position(int x, int y);
    void ensure_visible_position(const DockManager::SlidingParentInfo* parent = nullptr);
    DockManager::PanelInfo build_panel_info(bool force_layout) const;
    std::unique_ptr<DockableCollapsible> panel_;
    AssetListView list_view_;
    Callback cb_;
    MultiSelectCallback multi_select_cb_;
    std::vector<Asset> all_;
    std::vector<Result> results_;
    std::unordered_set<std::string> selected_values_;
    std::unique_ptr<class DMButton> multi_apply_btn_;
    std::unique_ptr<class ButtonWidget> multi_apply_btn_widget_;
    std::unique_ptr<class DMButton> multi_cancel_btn_;
    std::unique_ptr<class ButtonWidget> multi_cancel_btn_widget_;
    bool multi_select_mode_ = false;
    std::uint64_t tag_data_version_ = 0;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    std::unique_ptr<devmode::core::ManifestStore> owned_manifest_store_;
    Assets* assets_ = nullptr;
    int screen_w_ = 1920;
    int screen_h_ = 1080;
    SDL_Point last_known_position_{64, 64};
    SDL_Point pending_position_{64, 64};
    bool has_pending_position_ = false;
    bool has_custom_position_ = false;
    std::string floating_stack_key_;
    bool embedded_ = false;
    SDL_Rect embedded_rect_{0, 0, 0, 0};
    ExtraResultsProvider extra_results_provider_{};
    AssetFilter asset_filter_{};
};


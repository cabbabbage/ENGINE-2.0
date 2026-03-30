#pragma once

#include "asset_info.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

class AssetLibrary {

	public:
    struct RefreshMissingResult {
        std::vector<std::string> added_assets;
        std::vector<std::string> repaired_assets;
        std::vector<std::string> loaded_assets;
        std::vector<std::string> failed_assets;
    };

    explicit AssetLibrary(bool auto_load = true);
    void load_all_from_resources();
    std::vector<std::string> sync_missing_from_resources();
    void add_asset(const std::string& name, const nlohmann::json& metadata);
    std::shared_ptr<AssetInfo> get(const std::string& name) const;
    const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& all() const;
    std::vector<std::string> names() const;
    void loadAllAnimations(SDL_Renderer* renderer);
    void ensureAllAnimationsLoaded(SDL_Renderer* renderer);
    void ensureAnimationsLoadedFor(SDL_Renderer* renderer, const std::unordered_set<std::string>& names);
    void loadAnimationsFor(SDL_Renderer* renderer, const std::unordered_set<std::string>& names);
    RefreshMissingResult repairAndRefreshMissing(SDL_Renderer* renderer);
    bool remove(const std::string& name);

private:
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> info_by_name_;
    std::unordered_set<std::string> runtime_loaded_assets_;
    bool startup_warmup_complete_ = false;
};

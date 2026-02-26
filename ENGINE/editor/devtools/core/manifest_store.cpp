#include "devtools/core/manifest_store.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <limits>

#include "devtools/core/dev_json_store.hpp"
#include "devtools/tag_utils.hpp"

namespace devmode::core {
namespace {
std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
}

ManifestStore::AssetEditSession::AssetEditSession(ManifestStore* owner,
                                                  std::string name,
                                                  nlohmann::json draft,
                                                  bool is_new_asset,
                                                  std::uint64_t generation)
    : owner_(owner),
      name_(std::move(name)),
      draft_(std::move(draft)),
      is_new_(is_new_asset),
      generation_(generation) {}

bool ManifestStore::AssetEditSession::commit() {
    if (!owner_) {
        return false;
    }
    ManifestStore* owner = owner_;
    owner_ = nullptr;
    return owner->apply_edit(name_, draft_, generation_);
}

void ManifestStore::AssetEditSession::cancel() {
    owner_ = nullptr;
}

ManifestStore::AssetTransaction::AssetTransaction(ManifestStore* owner,
                                                  std::string name,
                                                  nlohmann::json draft,
                                                  bool is_new_asset,
                                                  std::uint64_t generation)
    : owner_(owner),
      name_(std::move(name)),
      draft_(std::move(draft)),
      is_new_(is_new_asset),
      generation_(generation) {}

bool ManifestStore::AssetTransaction::save() {
    if (!owner_) {
        return false;
    }
    if (!owner_->apply_edit(name_, draft_, generation_)) {
        return false;
    }
    generation_ = owner_->cache_generation_;
    return true;
}

bool ManifestStore::AssetTransaction::finalize() {
    if (!save()) {
        return false;
    }
    owner_ = nullptr;
    return true;
}

void ManifestStore::AssetTransaction::cancel() {
    owner_ = nullptr;
}

ManifestStore::ManifestStore()
    : ManifestStore(manifest::manifest_path(),
                    []() { return manifest::load_manifest(); },
                    [](const std::filesystem::path& path, const nlohmann::json& data, int indent) {
                        DevJsonStore::instance().submit(path, data, indent);
                    },
                    []() { DevJsonStore::instance().flush_all(); },
                    2) {}

ManifestStore::ManifestStore(const std::filesystem::path& manifest_path,
                             std::function<manifest::ManifestData()> loader,
                             std::function<void(const std::filesystem::path&, const nlohmann::json&, int)> submit,
                             std::function<void()> flush,
                             int indent)
    : manifest_path_(manifest_path),
      loader_(std::move(loader)),
      submit_(std::move(submit)),
      flush_(std::move(flush)),
      indent_(indent) {
    if (!submit_) {
        submit_ = [](const std::filesystem::path& path, const nlohmann::json& data, int indent) {
            DevJsonStore::instance().submit(path, data, indent);
        };
    }
    if (!flush_) {
        flush_ = []() { DevJsonStore::instance().flush_all(); };
    }
}

std::optional<std::string> ManifestStore::resolve_asset_name(const std::string& name) {
    ensure_loaded();

    if (!manifest_cache_.contains("assets") || !manifest_cache_["assets"].is_object()) {
        return std::nullopt;
    }

    nlohmann::json& assets = manifest_cache_["assets"];
    auto direct = assets.find(name);
    if (direct != assets.end()) {
        return name;
    }

    std::string needle = to_lower(name);
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        if (to_lower(it.key()) == needle) {
            return it.key();
        }
    }
    return std::nullopt;
}

ManifestStore::AssetView ManifestStore::get_asset(const std::string& name) {
    ensure_loaded();

    auto resolved = resolve_asset_name(name);
    if (!resolved) {
        return {};
    }

    nlohmann::json& assets = manifest_cache_["assets"];
    auto it = assets.find(*resolved);
    if (it == assets.end()) {
        return {};
    }
    return AssetView{*resolved, &(*it)};
}

ManifestStore::AssetEditSession ManifestStore::begin_asset_edit(const std::string& name,
                                                                bool create_if_missing) {
    ensure_loaded();
    ensure_asset_container();

    auto resolved = resolve_asset_name(name);
    if (!resolved && !create_if_missing) {
        return {};
    }

    const bool is_new_asset = !resolved.has_value();
    std::string target_name = resolved.has_value() ? *resolved : name;

    nlohmann::json existing = nlohmann::json::object();
    nlohmann::json& assets = manifest_cache_["assets"];
    if (!is_new_asset) {
        auto it = assets.find(target_name);
        if (it == assets.end()) {
            return {};
        }
        existing = *it;
    }

    const std::uint64_t generation = cache_generation_;
    return AssetEditSession(this, std::move(target_name), std::move(existing), is_new_asset, generation);
}

ManifestStore::AssetTransaction ManifestStore::begin_asset_transaction(const std::string& name,
                                                                       bool create_if_missing) {
    ensure_loaded();
    ensure_asset_container();

    auto resolved = resolve_asset_name(name);
    if (!resolved && !create_if_missing) {
        return {};
    }

    const bool is_new_asset = !resolved.has_value();
    std::string target_name = resolved.has_value() ? *resolved : name;

    nlohmann::json existing = nlohmann::json::object();
    nlohmann::json& assets = manifest_cache_["assets"];
    if (!is_new_asset) {
        auto it = assets.find(target_name);
        if (it == assets.end()) {
            return {};
        }
        existing = *it;
    }

    const std::uint64_t generation = cache_generation_;
    return AssetTransaction(this, std::move(target_name), std::move(existing), is_new_asset, generation);
}

bool ManifestStore::remove_asset(const std::string& name) {
    ensure_loaded();
    ensure_asset_container();

    auto resolved = resolve_asset_name(name);
    if (!resolved) {
        return false;
    }

    nlohmann::json& assets = manifest_cache_["assets"];
    auto erased = assets.erase(*resolved);
    if (erased == 0) {
        return false;
    }

    mark_dirty();
    if (submit_) {
        submit_(manifest_path_, manifest_cache_, indent_);
    }
    return true;
}

void ManifestStore::reload() {
    loaded_ = false;
    dirty_ = false;
    manifest_cache_ = nlohmann::json::object();
    state_ = CacheState::Unloaded;
    pending_reload_version_.reset();
    last_known_tag_version_ = std::numeric_limits<std::uint64_t>::max();
    ensure_loaded();
}

void ManifestStore::flush() {
    if (!flush_) {
        return;
    }

    if (dirty_) {
        state_ = CacheState::Flushing;
    }

    flush_();
    dirty_ = false;

    if (pending_reload_version_) {
        state_ = CacheState::Reloadable;
    } else {
        state_ = CacheState::Clean;
    }

    refresh_from_disk_if_safe();
}

const nlohmann::json& ManifestStore::manifest_json() {
    ensure_loaded();
    return manifest_cache_;
}

std::vector<ManifestStore::AssetView> ManifestStore::assets() {
    ensure_loaded();
    std::vector<AssetView> views;
    if (!manifest_cache_.contains("assets") || !manifest_cache_["assets"].is_object()) {
        return views;
    }
    nlohmann::json& assets_json = manifest_cache_["assets"];
    views.reserve(assets_json.size());
    for (auto it = assets_json.begin(); it != assets_json.end(); ++it) {
        views.push_back(AssetView{it.key(), &(*it)});
    }
    return views;
}

bool ManifestStore::update_map_entry(const std::string& map_id, const nlohmann::json& payload) {
    if (map_id.empty()) {
        return false;
    }
    ensure_loaded();
    ensure_asset_container();
    const std::uint64_t generation = cache_generation_;
    return apply_map_edit(map_id, payload, generation);
}

const nlohmann::json* ManifestStore::find_map_entry(const std::string& map_id) const {
    if (map_id.empty()) {
        return nullptr;
    }
    auto* self = const_cast<ManifestStore*>(this);
    self->ensure_loaded();
    const auto maps_it = self->manifest_cache_.find("maps");
    if (maps_it == self->manifest_cache_.end() || !maps_it->is_object()) {
        return nullptr;
    }
    const auto it = maps_it->find(map_id);
    if (it == maps_it->end()) {
        return nullptr;
    }
    return &(*it);
}

void ManifestStore::ensure_loaded() {
    ensure_loaded_once();
    refresh_from_disk_if_safe();
}

void ManifestStore::ensure_loaded_once() {
    if (loaded_) {
        return;
    }

    manifest::ManifestData data = loader_ ? loader_() : manifest::load_manifest();
    manifest_cache_ = data.raw;
    if (!manifest_cache_.is_object()) {
        manifest_cache_ = nlohmann::json::object();
    }
    ensure_asset_container();
    loaded_ = true;
    dirty_ = false;
    state_ = CacheState::Clean;
    ++cache_generation_;
    last_known_tag_version_ = tag_utils::tag_version();
    pending_reload_version_.reset();
}

void ManifestStore::refresh_from_disk_if_safe() {
    const std::uint64_t current_version = tag_utils::tag_version();
    if (current_version != last_known_tag_version_) {
        pending_reload_version_ = current_version;
    }

    const bool pending_write = has_pending_manifest_write();
    if (state_ == CacheState::Dirty || state_ == CacheState::Flushing) {
        return;
    }

    if (pending_write) {
        if (pending_reload_version_ && state_ == CacheState::Clean) {
            state_ = CacheState::Reloadable;
        }
        return;
    }

    if (!pending_reload_version_) {
        if (state_ == CacheState::Reloadable) {
            state_ = CacheState::Clean;
        }
        return;
    }

    manifest::ManifestData data = loader_ ? loader_() : manifest::load_manifest();
    manifest_cache_ = data.raw;
    if (!manifest_cache_.is_object()) {
        manifest_cache_ = nlohmann::json::object();
    }
    ensure_asset_container();
    loaded_ = true;
    dirty_ = false;
    state_ = CacheState::Clean;
    ++cache_generation_;
    last_known_tag_version_ = *pending_reload_version_;
    pending_reload_version_.reset();
}

bool ManifestStore::apply_edit(const std::string& name,
                               const nlohmann::json& payload,
                               std::uint64_t expected_generation) {
    ensure_loaded();
    ensure_asset_container();

    if (expected_generation != cache_generation_) {
        refresh_from_disk_if_safe();
        if (expected_generation != cache_generation_) {
            return false;
        }
    }

    manifest_cache_["assets"][name] = payload;
    mark_dirty();
    if (submit_) {
        submit_(manifest_path_, manifest_cache_, indent_);
    }
    return true;
}

bool ManifestStore::apply_map_edit(const std::string& name,
                                   const nlohmann::json& payload,
                                   std::uint64_t expected_generation) {
    ensure_loaded();
    ensure_asset_container();

    if (expected_generation != cache_generation_) {
        refresh_from_disk_if_safe();
        if (expected_generation != cache_generation_) {
            return false;
        }
    }

    manifest_cache_["maps"][name] = payload;
    mark_dirty();
    if (submit_) {
        submit_(manifest_path_, manifest_cache_, indent_);
    }
    return true;
}

void ManifestStore::mark_dirty() {
    dirty_ = true;
    state_ = CacheState::Dirty;
    ++cache_generation_;
}

bool ManifestStore::has_pending_manifest_write() const {
    return DevJsonStore::instance().has_pending_write(manifest_path_);
}

void ManifestStore::ensure_asset_container() {
    if (!manifest_cache_.contains("assets") || !manifest_cache_["assets"].is_object()) {
        manifest_cache_["assets"] = nlohmann::json::object();
    }
    if (!manifest_cache_.contains("maps")) {
        manifest_cache_["maps"] = nlohmann::json::object();
    }
}

}


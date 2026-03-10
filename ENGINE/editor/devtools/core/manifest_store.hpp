#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>

#include <nlohmann/json.hpp>

#include "core/manifest/manifest_loader.hpp"

namespace devmode::core {

class ManifestStore {
public:
    struct Telemetry {
        std::uint64_t asset_writes = 0;
        std::uint64_t map_writes = 0;
        std::uint64_t unguarded_writes = 0;
    };

    class ScopedWriteGuard {
    public:
        ScopedWriteGuard() = default;
        ScopedWriteGuard(ScopedWriteGuard&& other) noexcept;
        ScopedWriteGuard& operator=(ScopedWriteGuard&& other) noexcept;
        ScopedWriteGuard(const ScopedWriteGuard&) = delete;
        ScopedWriteGuard& operator=(const ScopedWriteGuard&) = delete;
        ~ScopedWriteGuard();

        explicit operator bool() const { return owner_ != nullptr; }

    private:
        friend class ManifestStore;
        ScopedWriteGuard(ManifestStore* owner, std::string reason);
        ManifestStore* owner_ = nullptr;
        std::string reason_;
    };

    enum class CacheState {
        Unloaded,
        Clean,
        Dirty,
        Flushing,
        Reloadable,
    };

    class AssetEditSession {
    public:
        AssetEditSession() = default;
        AssetEditSession(AssetEditSession&&) noexcept = default;
        AssetEditSession& operator=(AssetEditSession&&) noexcept = default;

        AssetEditSession(const AssetEditSession&) = delete;
        AssetEditSession& operator=(const AssetEditSession&) = delete;

        explicit operator bool() const { return owner_ != nullptr; }

        const std::string& name() const { return name_; }
        bool is_new_asset() const { return is_new_; }

        nlohmann::json& data() { return draft_; }
        const nlohmann::json& data() const { return draft_; }

        bool commit();
        void cancel();

    private:
        friend class ManifestStore;
        AssetEditSession(ManifestStore* owner,
                         std::string name,
                         nlohmann::json draft,
                         bool is_new_asset,
                         std::uint64_t generation);

        ManifestStore* owner_ = nullptr;
        std::string name_;
        nlohmann::json draft_;
        bool is_new_ = false;
        std::uint64_t generation_ = 0;
};

    class AssetTransaction {
    public:
        AssetTransaction() = default;
        AssetTransaction(AssetTransaction&&) noexcept = default;
        AssetTransaction& operator=(AssetTransaction&&) noexcept = default;

        AssetTransaction(const AssetTransaction&) = delete;
        AssetTransaction& operator=(const AssetTransaction&) = delete;

        explicit operator bool() const { return owner_ != nullptr; }

        nlohmann::json& data() { return draft_; }
        const nlohmann::json& data() const { return draft_; }

        bool save();
        bool finalize();
        void cancel();

    private:
        friend class ManifestStore;
        AssetTransaction(ManifestStore* owner,
                         std::string name,
                         nlohmann::json draft,
                         bool is_new_asset,
                         std::uint64_t generation);

        ManifestStore* owner_ = nullptr;
        std::string name_;
        nlohmann::json draft_;
        bool is_new_ = false;
        std::uint64_t generation_ = 0;
};

    struct AssetView {
        std::string name;
        const nlohmann::json* data = nullptr;

        explicit operator bool() const { return data != nullptr; }
        const nlohmann::json* operator->() const { return data; }
        const nlohmann::json& operator*() const { return *data; }
};

    ManifestStore();

    ManifestStore(const std::filesystem::path& manifest_path,
                  std::function<manifest::ManifestData()> loader,
                  std::function<void(const std::filesystem::path&, const nlohmann::json&, int)> submit = {},
                  std::function<void()> flush = {},
                  int indent = 2);

    std::optional<std::string> resolve_asset_name(const std::string& name);
    AssetView get_asset(const std::string& name);

    AssetEditSession begin_asset_edit(const std::string& name, bool create_if_missing = false);
    AssetTransaction begin_asset_transaction(const std::string& name, bool create_if_missing = false);

    bool remove_asset(const std::string& name);

    void reload();
    void flush();

    bool dirty() const { return dirty_; }
    const nlohmann::json& manifest_json();
    std::vector<AssetView> assets();

    struct MapPersistOptions {
        bool flush = true;
        std::string guard_reason;
    };

    bool persist_map_entry(const std::string& map_id,
                           const nlohmann::json& payload,
                           MapPersistOptions options = {});
    bool update_map_entry(const std::string& map_id, const nlohmann::json& payload);
    const nlohmann::json* find_map_entry(const std::string& map_id) const;

    ScopedWriteGuard scoped_guard(std::string reason = {});
    void set_require_write_guard(bool required, bool fail_on_violation = false);
    void set_write_violation_sink(std::function<void(const std::string&, const std::string&, const std::string&)> sink);
    Telemetry telemetry() const { return telemetry_; }
    void reset_telemetry();

private:
    void ensure_loaded();
    void ensure_loaded_once();
    void refresh_from_disk_if_safe();

    bool apply_edit(const std::string& name,
                    const nlohmann::json& payload,
                    std::uint64_t expected_generation);
    bool apply_map_edit(const std::string& name,
                        const nlohmann::json& payload,
                        std::uint64_t expected_generation);
    void ensure_asset_container();
    void mark_dirty();
    bool has_pending_manifest_write() const;
    bool guard_allows_write(const char* kind, const std::string& key);

    std::filesystem::path manifest_path_;
    std::function<manifest::ManifestData()> loader_;
    std::function<void(const std::filesystem::path&, const nlohmann::json&, int)> submit_;
    std::function<void()> flush_;
    int indent_ = 2;

    bool loaded_ = false;
    bool dirty_ = false;
    CacheState state_ = CacheState::Unloaded;
    nlohmann::json manifest_cache_ = nlohmann::json::object();
    std::uint64_t last_known_tag_version_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cache_generation_ = 0;
    std::optional<std::uint64_t> pending_reload_version_;

    bool require_guard_ = false;
    bool fail_on_guard_violation_ = false;
    int write_guard_depth_ = 0;
    std::vector<std::string> guard_reasons_;
    Telemetry telemetry_;
    std::function<void(const std::string&, const std::string&, const std::string&)> violation_sink_;
};

}

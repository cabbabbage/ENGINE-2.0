#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace devmode::core { class ManifestStore; }

namespace RoomManifestAdapter {

using ManifestWriter = std::function<void(const std::string&, const nlohmann::json&)>;

struct SyncContext {
    std::string data_section;
    std::string room_name;
    std::string manifest_map_id;
    devmode::core::ManifestStore* manifest_store = nullptr;
    nlohmann::json* map_info_root = nullptr;
    ManifestWriter manifest_writer{};
};

class SyncBoundary {
public:
    virtual ~SyncBoundary() = default;
    virtual bool enabled() const = 0;
};

class RuntimeSyncBoundary final : public SyncBoundary {
public:
    bool enabled() const override { return false; }
};

class EditorSyncBoundary final : public SyncBoundary {
public:
    bool enabled() const override { return true; }
};

nlohmann::json normalize_room_snapshot(const nlohmann::json& room_snapshot,
                                       int camera_height_px,
                                       float camera_tilt_deg,
                                       int camera_zoom_percent,
                                       int camera_center_dx,
                                       int camera_center_dz);

nlohmann::json build_payload_from_snapshot(const SyncContext& ctx, const nlohmann::json& normalized_room_snapshot);
bool write_payload(const SyncContext& ctx, const nlohmann::json& payload, const char* guard_reason);

} // namespace RoomManifestAdapter

#include "room_manifest_adapter.hpp"

#include "devtools/core/manifest_store.hpp"
#include <iostream>

namespace RoomManifestAdapter {

nlohmann::json normalize_room_snapshot(const nlohmann::json& room_snapshot,
                                       int camera_height_px,
                                       float camera_tilt_deg,
                                       int camera_zoom_percent,
                                       int camera_center_dx,
                                       int camera_center_dz) {
    nlohmann::json normalized = room_snapshot.is_object() ? room_snapshot : nlohmann::json::object();
    normalized["camera_height_px"] = camera_height_px;
    normalized["camera_tilt_deg"] = camera_tilt_deg;
    normalized["camera_zoom_percent"] = camera_zoom_percent;
    normalized["camera_center_dx"] = camera_center_dx;
    normalized["camera_center_dz"] = camera_center_dz;
    return normalized;
}

nlohmann::json build_payload_from_snapshot(const SyncContext& ctx, const nlohmann::json& normalized_room_snapshot) {
    nlohmann::json payload;
    if (ctx.map_info_root) {
        payload = *ctx.map_info_root;
    } else if (ctx.manifest_store && !ctx.manifest_map_id.empty()) {
        if (const nlohmann::json* entry = ctx.manifest_store->find_map_entry(ctx.manifest_map_id)) {
            payload = *entry;
        }
    }

    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }
    nlohmann::json& section = payload[ctx.data_section];
    if (!section.is_object()) {
        section = nlohmann::json::object();
    }
    section[ctx.room_name] = normalized_room_snapshot;
    return payload;
}

bool write_payload(const SyncContext& ctx, const nlohmann::json& payload, const char* guard_reason) {
    if (!payload.is_object()) {
        return false;
    }
    if (ctx.manifest_writer && !ctx.manifest_map_id.empty()) {
        ctx.manifest_writer(ctx.manifest_map_id, payload);
        return true;
    }
    if (ctx.manifest_store && !ctx.manifest_map_id.empty()) {
        devmode::core::ManifestStore::MapPersistOptions options;
        options.flush = false;
        options.guard_reason = guard_reason ? guard_reason : "RoomManifestAdapter::write_payload";
        const bool ok = ctx.manifest_store->persist_map_entry(ctx.manifest_map_id, payload, options);
        if (!ok) {
            std::cerr << "[Room] Failed to persist map entry for '" << ctx.manifest_map_id << "'\n";
        }
        return ok;
    }
    return true;
}

} // namespace RoomManifestAdapter

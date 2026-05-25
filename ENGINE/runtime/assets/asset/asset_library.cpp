#include "asset_library.hpp"
#include "primary_asset_cache.hpp"
#include "core/manifest/manifest_loader.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <unordered_set>
#include <vector>
#include "utils/log.hpp"
#include "gameplay/spawn/spawn_group_codec.hpp"

namespace {

std::filesystem::path assets_root_path() {
        return (std::filesystem::path("resources") / "assets").lexically_normal();
}

struct AnimationFolderInfo {
        std::string name;
        std::string relative_path;
        int frame_count = 0;
};

std::string to_lower_copy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
        });
        return value;
}

bool is_reserved_animation_name(const std::string& raw_name) {
        if (raw_name.empty()) {
                return true;
        }
        const std::string name = to_lower_copy(raw_name);
        static const std::unordered_set<std::string> reserved{
            "scaling_profile",
            "scaling-profile",
            "cache",
            "caches",
            "areas",
};
        return reserved.find(name) != reserved.end();
}

constexpr const char* kMovementEnabledKey = "movement_enabled";
constexpr const char* kAttackBoxEnabledKey = "attack_box_enabled";
constexpr const char* kHitboxEnabledKey = "hitbox_enabled";
constexpr const char* kImpassableEnabledKey = "impassable_enabled";
constexpr const char* kImpassableShapesKey = "impassable_shapes";
constexpr const char* kFloorBoxesEnabledKey = "floor_boxes_enabled";
constexpr const char* kFloorBoxesKey = "floor_boxes";
constexpr const char* kBoundaryTag = "boundary";
constexpr const char* kFloorBoxCandidateKey = "candidate";
constexpr const char* kFloorBoxCandidateCandidatesKey = "candidates";
constexpr const char* kFloorBoxCandidateGridResolutionKey = "grid_resolution";
constexpr int kFloorBoxCandidateGridResolutionMin = 2;
constexpr int kFloorBoxCandidateGridResolutionMax = 8;
constexpr int kFloorBoxCandidateGridResolutionDefault = 4;

int sanitize_floor_box_grid_resolution(int value) {
        int clamped = vibble::grid::clamp_resolution(value);
        clamped = std::clamp(clamped, kFloorBoxCandidateGridResolutionMin, kFloorBoxCandidateGridResolutionMax);
        return clamped;
}

std::string sanitize_floor_box_token(std::string value) {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
                const unsigned char uch = static_cast<unsigned char>(ch);
                if (std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.') {
                        out.push_back(static_cast<char>(std::tolower(uch)));
                } else if (std::isspace(uch) != 0) {
                        out.push_back('_');
                }
        }
        return out;
}

nlohmann::json normalize_floor_boxes_payload(const nlohmann::json& payload) {
        nlohmann::json normalized = nlohmann::json::array();
        if (!payload.is_array()) {
                return normalized;
        }

        std::unordered_set<std::string> used_ids;
        for (std::size_t index = 0; index < payload.size(); ++index) {
                const auto& entry = payload[index];
                if (!entry.is_object()) {
                        continue;
                }

                std::string id = sanitize_floor_box_token(entry.value("id", std::string{}));
                if (id.empty()) {
                        id = std::string("floor_box_") + std::to_string(index + 1);
                }
                std::string unique_id = id;
                for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
                        unique_id = id + "_" + std::to_string(suffix);
                }

                std::string name = entry.value("name", std::string{});
                if (name.empty()) {
                        name = std::string("floor_box_") + std::to_string(index + 1);
                }

                auto finite_or = [](double value, double fallback) {
                        return std::isfinite(value) ? value : fallback;
                };

                std::unordered_set<std::string> seen_tags;
                std::vector<std::string> normalized_tags;
                auto append_tag = [&](const std::string& raw_tag) {
                        std::string tag = raw_tag;
                        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) {
                                return static_cast<char>(std::tolower(c));
                        });
                        if (tag.empty()) {
                                return;
                        }
                        if (seen_tags.insert(tag).second) {
                                normalized_tags.push_back(std::move(tag));
                        }
                };
                if (entry.contains("tags")) {
                        const auto& tags_payload = entry["tags"];
                        if (tags_payload.is_array()) {
                                for (const auto& tag_value : tags_payload) {
                                        if (tag_value.is_string()) {
                                                append_tag(tag_value.get<std::string>());
                                        }
                                }
                        } else if (tags_payload.is_string()) {
                                append_tag(tags_payload.get<std::string>());
                        }
                }
                normalized_tags.erase(
                    std::remove(normalized_tags.begin(), normalized_tags.end(), std::string(kBoundaryTag)),
                    normalized_tags.end());

                nlohmann::json canonical = nlohmann::json::object();
                canonical["id"] = unique_id;
                canonical["name"] = name;
                canonical["position_x"] = finite_or(entry.value("position_x", 0.0), 0.0);
                canonical["position_z"] = finite_or(entry.value("position_z", 0.0), 0.0);
                canonical["width"] = std::max(0.0, finite_or(entry.value("width", 0.0), 0.0));
                canonical["depth"] = std::max(0.0, finite_or(entry.value("depth", 0.0), 0.0));
                canonical["enabled"] = entry.value("enabled", true);
                canonical["tags"] = normalized_tags;
                if (entry.contains(kFloorBoxCandidateKey)) {
                        nlohmann::json candidate = nlohmann::json::object();
                        const auto& raw_candidate = entry[kFloorBoxCandidateKey];
                        if (raw_candidate.is_object()) {
                                if (raw_candidate.contains(kFloorBoxCandidateCandidatesKey)) {
                                        candidate[kFloorBoxCandidateCandidatesKey] =
                                            raw_candidate[kFloorBoxCandidateCandidatesKey];
                                }
                                candidate[kFloorBoxCandidateGridResolutionKey] =
                                    sanitize_floor_box_grid_resolution(
                                        vibble::spawn_group_codec::read_int_field(
                                            raw_candidate,
                                            kFloorBoxCandidateGridResolutionKey,
                                            kFloorBoxCandidateGridResolutionDefault));
                        } else {
                                candidate[kFloorBoxCandidateGridResolutionKey] =
                                    kFloorBoxCandidateGridResolutionDefault;
                        }
                        vibble::spawn_group_codec::sanitize_spawn_group_candidates(candidate);
                        canonical[kFloorBoxCandidateKey] = std::move(candidate);
                }
                normalized.push_back(std::move(canonical));
        }
        return normalized;
}

int read_impassable_int(const nlohmann::json& node, const char* key, int fallback) {
        if (!node.is_object() || !node.contains(key)) {
                return fallback;
        }
        const auto& value = node[key];
        if (value.is_number_integer()) {
                return value.get<int>();
        }
        if (value.is_number_float()) {
                return static_cast<int>(std::lround(value.get<double>()));
        }
        return fallback;
}

long long shape_signed_area_twice(const std::vector<std::pair<int, int>>& points) {
        if (points.size() < 3) {
                return 0;
        }
        long long area2 = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % points.size()];
                area2 += static_cast<long long>(a.first) * static_cast<long long>(b.second) -
                         static_cast<long long>(b.first) * static_cast<long long>(a.second);
        }
        return area2;
}

nlohmann::json normalize_impassable_shapes_payload(const nlohmann::json& payload) {
        nlohmann::json normalized = nlohmann::json::array();
        if (!payload.is_array()) {
                return normalized;
        }

        std::unordered_set<std::string> used_ids;
        std::unordered_set<std::string> used_names;
        for (std::size_t index = 0; index < payload.size(); ++index) {
                const auto& entry = payload[index];
                if (!entry.is_object()) {
                        continue;
                }

                std::string id = sanitize_floor_box_token(entry.value("id", std::string{}));
                if (id.empty()) {
                        id = std::string("impassable_shape_") + std::to_string(index + 1);
                }
                std::string unique_id = id;
                for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
                        unique_id = id + "_" + std::to_string(suffix);
                }

                std::string name = entry.value("name", std::string{});
                if (name.empty()) {
                        name = std::string("Impassable Shape ") + std::to_string(index + 1);
                }
                std::string unique_name = name;
                for (int suffix = 2; !used_names.insert(unique_name).second; ++suffix) {
                        unique_name = name + " " + std::to_string(suffix);
                }

                std::vector<std::pair<int, int>> points;
                if (entry.contains("points") && entry["points"].is_array()) {
                        for (const auto& point_node : entry["points"]) {
                                if (!point_node.is_object()) {
                                        continue;
                                }
                                const int x = read_impassable_int(point_node, "x", 0);
                                const int y = read_impassable_int(point_node, "y", 0);
                                points.emplace_back(x, y);
                        }
                }
                if (points.size() < 3) {
                        continue;
                }
                if (shape_signed_area_twice(points) < 0) {
                        std::reverse(points.begin(), points.end());
                }

                nlohmann::json canonical = nlohmann::json::object();
                canonical["id"] = unique_id;
                canonical["name"] = unique_name;
                canonical["enabled"] = entry.value("enabled", true);
                nlohmann::json normalized_points = nlohmann::json::array();
                for (const auto& point : points) {
                        normalized_points.push_back(nlohmann::json::object({
                            {"x", point.first},
                            {"y", point.second},
                        }));
                }
                canonical["points"] = std::move(normalized_points);
                normalized.push_back(std::move(canonical));
        }
        return normalized;
}

bool ensure_bool_field(nlohmann::json& metadata, const char* key, bool default_value = false) {
        if (metadata.contains(key) && metadata[key].is_boolean()) {
                return false;
        }
        metadata[key] = default_value;
        return true;
}

bool normalize_animation_system_payload(nlohmann::json& animation_payload,
                                        bool movement_enabled,
                                        bool hitbox_enabled,
                                        bool attack_box_enabled) {
        if (!animation_payload.is_object()) {
                return false;
        }

        bool mutated = false;
        if (!movement_enabled) {
                mutated = animation_payload.erase("movement") > 0 || mutated;
                mutated = animation_payload.erase("movement_paths") > 0 || mutated;
                mutated = animation_payload.erase("movement_total") > 0 || mutated;
        }
        if (!hitbox_enabled) {
                mutated = animation_payload.erase("hit_boxes") > 0 || mutated;
        }
        if (!attack_box_enabled) {
                mutated = animation_payload.erase("attack_boxes") > 0 || mutated;
        }
        return mutated;
}

int count_png_frames(const std::filesystem::path& folder) {
        int count = 0;
        try {
                if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
                        return 0;
                }
                for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                        if (!entry.is_regular_file()) {
                                continue;
                        }
                        std::string ext = to_lower_copy(entry.path().extension().string());
                        if (ext == ".png") {
                                ++count;
                        }
                }
        } catch (const std::exception& ex) {
                vibble::log::warn(std::string("[AssetLibrary] Unable to enumerate '") + folder.generic_string() + "': " + ex.what());
                return 0;
        } catch (...) {
                vibble::log::warn(std::string("[AssetLibrary] Unknown error enumerating '") + folder.generic_string() + "'");
                return 0;
        }
        return count;
}

std::string summarize_names(std::vector<std::string> names, std::size_t max_names = 8) {
        if (names.empty()) {
                return "[]";
        }

        std::sort(names.begin(), names.end());
        std::ostringstream oss;
        oss << "[";
        const std::size_t count = std::min(max_names, names.size());
        for (std::size_t idx = 0; idx < count; ++idx) {
                if (idx > 0) {
                        oss << ", ";
                }
                oss << names[idx];
        }
        if (names.size() > count) {
                oss << ", +" << (names.size() - count) << " more";
        }
        oss << "]";
        return oss.str();
}

struct WarmupPassStats {
        std::vector<std::string> cache_created;
        std::vector<std::string> cache_rebuilt;
        std::vector<std::string> cache_repaired;
        std::vector<std::string> cache_reused;
        std::vector<std::string> cache_failed;
        std::vector<std::string> load_succeeded;
        std::vector<std::string> load_failed;
        std::size_t already_loaded = 0;
};

std::vector<AnimationFolderInfo> discover_animation_folders(const std::filesystem::path& asset_dir) {
        std::vector<AnimationFolderInfo> result;
        try {
                if (!std::filesystem::exists(asset_dir) || !std::filesystem::is_directory(asset_dir)) {
                        return result;
                }
        } catch (...) {
                return result;
        }

        std::unordered_set<std::string> seen;

        const int root_frames = count_png_frames(asset_dir);
        if (root_frames > 0) {
                seen.insert("default");
                result.push_back(AnimationFolderInfo{"default", "", root_frames});
        }

        try {
                for (const auto& entry : std::filesystem::directory_iterator(asset_dir)) {
                        if (!entry.is_directory()) {
                                continue;
                        }
                        std::string name = entry.path().filename().string();
                        if (name.empty()) {
                                continue;
                        }
                        if (name.front() == '.' || is_reserved_animation_name(name)) {
                                continue;
                        }
                        const int frames = count_png_frames(entry.path());
                        if (frames <= 0) {
                                continue;
                        }
                        if (seen.insert(name).second) {
                                result.push_back(AnimationFolderInfo{name, name, frames});
                        }
                }
        } catch (const std::exception& ex) {
                vibble::log::warn(std::string("[AssetLibrary] Failed to enumerate animations under '") + asset_dir.generic_string() + "': " + ex.what());
        } catch (...) {
                vibble::log::warn(std::string("[AssetLibrary] Unknown error enumerating animations under '") + asset_dir.generic_string() + "'");
        }

        std::sort(result.begin(), result.end(), [](const AnimationFolderInfo& lhs, const AnimationFolderInfo& rhs) {
                return lhs.name < rhs.name;
        });
        return result;
}

bool ensure_start_animation(nlohmann::json& metadata) {
        auto animations_it = metadata.find("animations");
        if (animations_it == metadata.end() || !animations_it->is_object()) {
                        return false;
        }
        const auto& animations = *animations_it;
        const auto is_valid = [&](const std::string& candidate) -> bool {
                if (candidate.empty()) {
                        return false;
                }
                if (is_reserved_animation_name(candidate)) {
                        return false;
                }
                auto it = animations.find(candidate);
                return it != animations.end() && it->is_object();
};

        if (metadata.contains("start") && metadata["start"].is_string()) {
                const std::string existing = metadata["start"].get<std::string>();
                if (is_valid(existing)) {
                        return false;
                }
        }

        const auto select = [&]() -> std::string {
                if (is_valid("default")) {
                        return "default";
                }
                if (is_valid("idle")) {
                        return "idle";
                }
                for (auto it = animations.begin(); it != animations.end(); ++it) {
                        if (is_valid(it.key())) {
                                return it.key();
                        }
                }
                return {};
};

        std::string replacement = select();
        if (replacement.empty()) {
                return false;
        }
        metadata["start"] = replacement;
        return true;
}

bool ensure_animation_metadata(const std::string& asset_name,
                               nlohmann::json& metadata,
                               const std::filesystem::path& assets_root) {
        const auto asset_dir = (assets_root / asset_name).lexically_normal();
        const auto folders = discover_animation_folders(asset_dir);

        bool mutated = false;
        if (!metadata.contains("animations") || !metadata["animations"].is_object()) {
                metadata["animations"] = nlohmann::json::object();
                mutated = true;
        }
        nlohmann::json& animations = metadata["animations"];

        if (folders.empty() && animations.empty()) {
                animations["default"] = nlohmann::json::object({
                    {"source", nlohmann::json::object({
                        {"kind", "folder"},
                        {"path", "default"},
                        {"name", ""}
                    })},
                    {"number_of_frames", 1},
                    {"locked", false},
                    {"on_end", "default"}
                });
                metadata["start"] = "default";
                return true;
        }

        for (const auto& folder : folders) {
                nlohmann::json& slot = animations[folder.name];
                if (!slot.is_object()) {
                        slot = nlohmann::json::object();
                        mutated = true;
                }

                if (!slot.contains("source") || !slot["source"].is_object()) {
                        slot["source"] = nlohmann::json::object();
                        mutated = true;
                }
                nlohmann::json& source = slot["source"];
                bool source_mutated = false;
                if (!source.contains("kind") || !source["kind"].is_string() || source["kind"].get<std::string>().empty()) {
                        source["kind"] = "folder";
                        source_mutated = true;
                }
                const std::string desired_path = folder.relative_path;
                if (!source.contains("path") || !source["path"].is_string() || source["path"].get<std::string>() != desired_path) {
                        source["path"] = desired_path;
                        source_mutated = true;
                }
                if (source_mutated) {
                        mutated = true;
                }

                if (!slot.contains("locked") || !slot["locked"].is_boolean()) {
                        slot["locked"] = false;
                        mutated = true;
                }
                if (slot.contains("loop")) {
                        slot.erase("loop");
                        mutated = true;
                }
                if (!slot.contains("on_end") || !slot["on_end"].is_string() || slot["on_end"].get<std::string>().empty()) {
                        slot["on_end"] = "default";
                        mutated = true;
                }
        }

        mutated |= ensure_start_animation(metadata);
        return mutated;
}

bool ensure_manifest_entry_shape(const std::string& asset_name,
                                 nlohmann::json& metadata,
                                 const std::filesystem::path& assets_root) {
        bool mutated = false;
        if (!metadata.is_object()) {
                metadata = nlohmann::json::object();
                mutated = true;
        }
        if (!metadata.contains("asset_name") || !metadata["asset_name"].is_string() || metadata["asset_name"].get<std::string>().empty()) {
                metadata["asset_name"] = asset_name;
                mutated = true;
        }
        const auto default_dir = (assets_root / asset_name).lexically_normal().generic_string();
        if (!metadata.contains("asset_directory") || !metadata["asset_directory"].is_string() || metadata["asset_directory"].get<std::string>().empty()) {
                metadata["asset_directory"] = default_dir;
                mutated = true;
        }
        mutated = ensure_bool_field(metadata, kMovementEnabledKey, false) || mutated;
        mutated = ensure_bool_field(metadata, kAttackBoxEnabledKey, false) || mutated;
        mutated = ensure_bool_field(metadata, kHitboxEnabledKey, false) || mutated;
        mutated = ensure_bool_field(metadata, kImpassableEnabledKey, false) || mutated;
        mutated = ensure_bool_field(metadata, kFloorBoxesEnabledKey, false) || mutated;

        const bool movement_enabled = metadata.value(kMovementEnabledKey, false);
        const bool attack_box_enabled = metadata.value(kAttackBoxEnabledKey, false);
        const bool hitbox_enabled = metadata.value(kHitboxEnabledKey, false);
        const bool impassable_enabled = metadata.value(kImpassableEnabledKey, false);
        const bool floor_boxes_enabled = metadata.value(kFloorBoxesEnabledKey, false);

        if (floor_boxes_enabled) {
                const nlohmann::json normalized_floor_boxes =
                    normalize_floor_boxes_payload(metadata.value(kFloorBoxesKey, nlohmann::json::array()));
                if (normalized_floor_boxes.empty()) {
                        mutated = metadata.erase(kFloorBoxesKey) > 0 || mutated;
                } else if (!metadata.contains(kFloorBoxesKey) || metadata[kFloorBoxesKey] != normalized_floor_boxes) {
                        metadata[kFloorBoxesKey] = normalized_floor_boxes;
                        mutated = true;
                }
        } else {
                mutated = metadata.erase(kFloorBoxesKey) > 0 || mutated;
        }

        mutated = metadata.erase("impassable_box_enabled") > 0 || mutated;
        mutated = metadata.erase("impassable_boxes") > 0 || mutated;
        if (impassable_enabled) {
                const nlohmann::json normalized_impassable_shapes =
                    normalize_impassable_shapes_payload(metadata.value(kImpassableShapesKey, nlohmann::json::array()));
                if (normalized_impassable_shapes.empty()) {
                        mutated = metadata.erase(kImpassableShapesKey) > 0 || mutated;
                } else if (!metadata.contains(kImpassableShapesKey) || metadata[kImpassableShapesKey] != normalized_impassable_shapes) {
                        metadata[kImpassableShapesKey] = normalized_impassable_shapes;
                        mutated = true;
                }
        } else {
                mutated = metadata.erase(kImpassableShapesKey) > 0 || mutated;
        }

        mutated |= ensure_animation_metadata(asset_name, metadata, assets_root);
        if (metadata.contains("animations") && metadata["animations"].is_object()) {
                for (auto it = metadata["animations"].begin(); it != metadata["animations"].end(); ++it) {
                        if (!it.value().is_object()) {
                                continue;
                        }
                        if (normalize_animation_system_payload(it.value(),
                                                              movement_enabled,
                                                              hitbox_enabled,
                                                              attack_box_enabled)) {
                                mutated = true;
                        }
                }
        }
        return mutated;
}

std::vector<std::string> discover_asset_directories(const std::filesystem::path& assets_root) {
        std::vector<std::string> names;
        try {
                if (!std::filesystem::exists(assets_root) || !std::filesystem::is_directory(assets_root)) {
                        return names;
                }
                for (const auto& entry : std::filesystem::directory_iterator(assets_root)) {
                        if (!entry.is_directory()) {
                                continue;
                        }
                        const std::string name = entry.path().filename().string();
                        if (!name.empty()) {
                                names.push_back(name);
                        }
                }
        } catch (const std::exception& ex) {
                vibble::log::warn(std::string("[AssetLibrary] Failed to enumerate assets root '") + assets_root.generic_string() + "': " + ex.what());
                names.clear();
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
}

bool load_manifest_with_asset_sync(manifest::ManifestData& manifest,
                                   const std::filesystem::path& assets_root,
                                   std::vector<std::string>* out_asset_names = nullptr) {
        try {
                manifest = manifest::load_manifest();
        } catch (const std::exception& error) {
                vibble::log::error(std::string("[AssetLibrary] Failed to load manifest: ") + error.what());
                return false;
        }

        const auto manifest_path = std::filesystem::absolute(std::filesystem::path(manifest::manifest_path()));
        vibble::log::info(std::string("[FrameData] Loading animations manifest from ") + manifest_path.generic_string());

        bool manifest_dirty = false;
        auto& raw_assets = manifest.raw["assets"];
        if (!raw_assets.is_object()) {
                raw_assets = nlohmann::json::object();
                manifest_dirty = true;
        }

        for (auto it = raw_assets.begin(); it != raw_assets.end(); ++it) {
                manifest_dirty |= ensure_manifest_entry_shape(it.key(), it.value(), assets_root);
        }

        const auto discovered_assets = discover_asset_directories(assets_root);
        if (discovered_assets.empty()) {
                std::error_code ec;
                const bool assets_root_exists = std::filesystem::exists(assets_root, ec);
                if (!assets_root_exists || ec) {
                        vibble::log::warn(std::string("[AssetLibrary] Assets root '") + assets_root.generic_string() + "' is missing or inaccessible.");
                }
        } else {
                for (const auto& asset_name : discovered_assets) {
                        nlohmann::json& metadata = raw_assets[asset_name];
                        manifest_dirty |= ensure_manifest_entry_shape(asset_name, metadata, assets_root);
                }
        }

        manifest.assets = raw_assets;
        if (manifest_dirty) {
                try {
                        manifest.raw["assets"] = raw_assets;
                        manifest::save_manifest(manifest);
                } catch (const std::exception& ex) {
                        vibble::log::warn(std::string("[AssetLibrary] Failed to persist manifest assets: ") + ex.what());
                } catch (...) {
                        vibble::log::warn("[AssetLibrary] Failed to persist manifest assets due to unknown error");
                }
        }

        if (out_asset_names) {
                out_asset_names->clear();
                out_asset_names->reserve(raw_assets.size());
                for (auto it = raw_assets.begin(); it != raw_assets.end(); ++it) {
                        out_asset_names->push_back(it.key());
                }
                std::sort(out_asset_names->begin(), out_asset_names->end());
        }
        return true;
}

std::vector<std::string> sorted_names_from_set(const std::unordered_set<std::string>& names) {
        std::vector<std::string> ordered;
        ordered.reserve(names.size());
        for (const auto& name : names) {
                if (!name.empty()) {
                        ordered.push_back(name);
                }
        }
        std::sort(ordered.begin(), ordered.end());
        ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
        return ordered;
}

void append_unique_names(std::vector<std::string>& out, const std::vector<std::string>& names) {
        std::unordered_set<std::string> seen(out.begin(), out.end());
        for (const auto& name : names) {
                if (!name.empty() && seen.insert(name).second) {
                        out.push_back(name);
                }
        }
        std::sort(out.begin(), out.end());
}

bool animation_cache_has_usable_texture(const Animation& animation) {
        if (animation.cached_frame_count() == 0) {
                return false;
        }
        for (const auto& frame : animation.cached_frames()) {
                for (SDL_Texture* texture : frame.textures) {
                        if (texture) {
                                return true;
                        }
                }
        }
        return false;
}

bool payload_is_folder_animation(const nlohmann::json& payload) {
        if (!payload.is_object() || !payload.contains("source") || !payload["source"].is_object()) {
                return false;
        }
        try {
                return payload["source"].value("kind", std::string{}) == "folder";
        } catch (...) {
                return false;
        }
}

bool asset_runtime_animation_cache_is_usable(const AssetInfo& info,
                                             std::vector<std::string>* missing_folder_animations = nullptr) {
        if (info.animations.empty()) {
                return false;
        }

        bool has_required_folder_animation = false;
        bool all_required_folder_animations_ready = true;
        for (const std::string& animation_name : info.animation_names()) {
                if (!payload_is_folder_animation(info.animation_payload(animation_name))) {
                        continue;
                }
                has_required_folder_animation = true;
                auto runtime_it = info.animations.find(animation_name);
                if (runtime_it == info.animations.end() ||
                    !animation_cache_has_usable_texture(runtime_it->second)) {
                        all_required_folder_animations_ready = false;
                        if (missing_folder_animations) {
                                missing_folder_animations->push_back(animation_name);
                        }
                }
        }

        return !has_required_folder_animation || all_required_folder_animations_ready;
}

WarmupPassStats warmup_assets(SDL_Renderer* renderer,
                              std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& info_by_name,
                              std::unordered_set<std::string>& runtime_loaded_assets,
                              const std::vector<std::string>& ordered_names,
                              const std::string& preload_label,
                              const std::string& load_label) {
        WarmupPassStats stats;
        if (!renderer || ordered_names.empty()) {
                return stats;
        }

        const auto begin = std::chrono::steady_clock::now();
        PrimaryAssetCache primary_cache(renderer);
        std::vector<std::shared_ptr<AssetInfo>> load_candidates;
        std::vector<AssetInfo*> repair_candidates;
        std::vector<std::string> reused_candidate_names;

        for (const auto& name : ordered_names) {
                auto it = info_by_name.find(name);
                if (it == info_by_name.end() || !it->second) {
                        stats.cache_failed.push_back(name);
                        vibble::log::warn(std::string("[AssetLibrary] ") + preload_label + " cache_failed '" + name + "' (missing AssetInfo)");
                        continue;
                }

                auto& info = it->second;
                if (runtime_loaded_assets.find(name) != runtime_loaded_assets.end() || !info->animations.empty()) {
                        runtime_loaded_assets.insert(name);
                        ++stats.already_loaded;
                        continue;
                }

                PrimaryAssetCache::WarmupOutcome outcome = PrimaryAssetCache::WarmupOutcome::Failed;
                if (!primary_cache.ensure_cache_ready(*info, nullptr, nullptr, &outcome, true)) {
                        stats.cache_failed.push_back(name);
                        vibble::log::warn(std::string("[AssetLibrary] ") + preload_label + " cache_failed '" + name + "'");
                        continue;
                }

                load_candidates.push_back(info);
                switch (outcome) {
                case PrimaryAssetCache::WarmupOutcome::Created:
                        stats.cache_created.push_back(name);
                        vibble::log::info(std::string("[AssetLibrary] ") + preload_label + " cache_created '" + name + "'");
                        break;
                case PrimaryAssetCache::WarmupOutcome::Rebuilt:
                        stats.cache_rebuilt.push_back(name);
                        vibble::log::info(std::string("[AssetLibrary] ") + preload_label + " cache_rebuilt '" + name + "'");
                        break;
                case PrimaryAssetCache::WarmupOutcome::Reused:
                        repair_candidates.push_back(info.get());
                        reused_candidate_names.push_back(name);
                        break;
                case PrimaryAssetCache::WarmupOutcome::Repaired:
                        stats.cache_repaired.push_back(name);
                        vibble::log::info(std::string("[AssetLibrary] ") + preload_label + " cache_repaired '" + name + "'");
                        break;
                case PrimaryAssetCache::WarmupOutcome::Failed:
                        stats.cache_failed.push_back(name);
                        vibble::log::warn(std::string("[AssetLibrary] ") + preload_label + " cache_failed '" + name + "'");
                        break;
                }
        }

        if (!repair_candidates.empty()) {
                auto detect_result = primary_cache.detect_missing_cache_files(repair_candidates);
                std::unordered_set<std::string> repaired_names;

                if (!detect_result.ok) {
                        vibble::log::warn(std::string("[AssetLibrary] ") + preload_label + " repair detection failed: " + detect_result.error);
                } else if (!detect_result.touched_assets.empty()) {
                        std::unordered_set<std::string> touched_set(detect_result.touched_assets.begin(),
                                                                    detect_result.touched_assets.end());
                        std::vector<AssetInfo*> touched_infos;
                        touched_infos.reserve(detect_result.touched_assets.size());
                        for (AssetInfo* info : repair_candidates) {
                                if (info && touched_set.find(info->name) != touched_set.end()) {
                                        touched_infos.push_back(info);
                                }
                        }

                        auto repair_result = primary_cache.repair_missing_cache_files(touched_infos);
                        if (!repair_result.ok) {
                                vibble::log::warn(std::string("[AssetLibrary] ") + preload_label + " batched repair failed: " + repair_result.error);
                        } else {
                                for (const auto& [asset_name, written_files] : repair_result.written_files_by_asset) {
                                        if (written_files.empty()) {
                                                continue;
                                        }
                                        repaired_names.insert(asset_name);
                                        stats.cache_repaired.push_back(asset_name);
                                        vibble::log::info(std::string("[AssetLibrary] ") + preload_label + " cache_repaired '" + asset_name + "'");
                                }
                        }
                }

                for (const auto& name : reused_candidate_names) {
                        if (repaired_names.find(name) != repaired_names.end()) {
                                continue;
                        }
                        stats.cache_reused.push_back(name);
                       // vibble::log::info(std::string("[AssetLibrary] ") + preload_label + " cache_reused '" + name + "'");
                }
        }

        const auto preload_end = std::chrono::steady_clock::now();
        const auto preload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(preload_end - begin).count();
        vibble::log::info(
            std::string("[AssetLibrary] ") + preload_label + " summary: created=" + std::to_string(stats.cache_created.size()) +
            " " + summarize_names(stats.cache_created) +
            ", rebuilt=" + std::to_string(stats.cache_rebuilt.size()) +
            " " + summarize_names(stats.cache_rebuilt) +
            ", repaired=" + std::to_string(stats.cache_repaired.size()) +
            " " + summarize_names(stats.cache_repaired) +
            ", reused=" + std::to_string(stats.cache_reused.size()) +
            " " + summarize_names(stats.cache_reused) +
            ", failed=" + std::to_string(stats.cache_failed.size()) +
            " " + summarize_names(stats.cache_failed) +
            ", already_loaded=" + std::to_string(stats.already_loaded) +
            " in " + std::to_string(preload_ms) + "ms");

        const auto load_begin = std::chrono::steady_clock::now();
        for (const auto& info : load_candidates) {
                if (!info) {
                        continue;
                }
                if (runtime_loaded_assets.find(info->name) != runtime_loaded_assets.end()) {
                        continue;
                }
                std::vector<std::string> missing_cached_folder_animations;
                if (asset_runtime_animation_cache_is_usable(*info, &missing_cached_folder_animations)) {
                        runtime_loaded_assets.insert(info->name);
                        continue;
                }
                if (!info->animations.empty() && !missing_cached_folder_animations.empty()) {
                        vibble::log::warn(std::string("[AssetLibrary] ") + load_label + " retrying '" + info->name +
                                          "' because selected folder animation(s) lack usable runtime frame textures. Missing/invalid animations: " +
                                          summarize_names(missing_cached_folder_animations));
                }
                try {
                        const auto item_begin = std::chrono::steady_clock::now();
                        const auto load_result = info->loadAnimationsDetailed(renderer, true, true, true);
                        const auto item_end = std::chrono::steady_clock::now();
                        const auto item_ms = std::chrono::duration_cast<std::chrono::milliseconds>(item_end - item_begin).count();
                        if (!load_result.ok()) {
                                stats.load_failed.push_back(info->name);
                                vibble::log::error(std::string("[AssetLibrary] ") + load_label + " failed for '" + info->name +
                                                   "': selected folder animation(s) did not produce usable runtime frame textures. Missing/invalid animations: " +
                                                   summarize_names(load_result.missing_runtime_frame_animations));
                                continue;
                        }
                        runtime_loaded_assets.insert(info->name);
                        stats.load_succeeded.push_back(info->name);
                        vibble::log::info(std::string("[AssetLibrary] ") + load_label + " loaded '" + info->name + "' in " +
                                          std::to_string(item_ms) + "ms");
                } catch (const std::exception& ex) {
                        stats.load_failed.push_back(info->name);
                        vibble::log::error(std::string("[AssetLibrary] ") + load_label + " failed for '" + info->name +
                                           "': " + ex.what());
                } catch (...) {
                        stats.load_failed.push_back(info->name);
                        vibble::log::error(std::string("[AssetLibrary] ") + load_label + " failed for '" + info->name +
                                           "' due to an unknown error.");
                }
        }
        const auto load_end = std::chrono::steady_clock::now();
        const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_begin).count();
        vibble::log::info(
            std::string("[AssetLibrary] ") + load_label + " summary: loaded=" +
            std::to_string(stats.load_succeeded.size()) + " " + summarize_names(stats.load_succeeded) +
            ", failed=" + std::to_string(stats.load_failed.size()) + " " + summarize_names(stats.load_failed) +
            ", already_loaded=" + std::to_string(stats.already_loaded) +
            " in " + std::to_string(load_ms) + "ms");

        return stats;
}

}

AssetLibrary::AssetLibrary(bool auto_load) {
        if (auto_load) {
                load_all_from_resources();
        }
}

void AssetLibrary::load_all_from_resources() {
        info_by_name_.clear();
        const auto assets_root = assets_root_path();
        manifest::ManifestData manifest;
        runtime_loaded_assets_.clear();
        startup_warmup_complete_ = false;
        if (!load_manifest_with_asset_sync(manifest, assets_root)) {
                return;
        }

        int loaded = 0;
        int failed = 0;
        const auto start_ms = std::chrono::steady_clock::now();

        struct AssetBuildJob {
                std::string name;
                nlohmann::json metadata;
};
        std::vector<AssetBuildJob> work_items;
        work_items.reserve(manifest.assets.size());

        for (auto it = manifest.assets.begin(); it != manifest.assets.end(); ++it) {
                const std::string name = it.key();
                const auto& metadata = it.value();

                if (!metadata.is_object()) {
                        ++failed;
                        vibble::log::warn(std::string("[AssetLibrary] Manifest entry for asset '") + name + "' is not a JSON object.");
                        continue;
                }

                work_items.push_back(AssetBuildJob{name, metadata});
        }

        if (!work_items.empty()) {
                const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
                const std::size_t worker_count = std::min(work_items.size(), static_cast<std::size_t>(hardware_threads));
                const std::size_t slice_size = (work_items.size() + worker_count - 1) / worker_count;

                struct WorkerResult {
                        int loaded = 0;
                        int failed = 0;
                        std::vector<std::pair<std::string, std::shared_ptr<AssetInfo>>> assets;
};

                std::vector<std::future<WorkerResult>> futures;
                futures.reserve(worker_count);

                for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                        const std::size_t start_index = worker_index * slice_size;
                        if (start_index >= work_items.size()) {
                                break;
                        }
                        const std::size_t end_index = std::min(work_items.size(), start_index + slice_size);
                        futures.push_back(std::async(std::launch::async,
                                                     [start_index, end_index, &work_items]() -> WorkerResult {
                                                             WorkerResult result;
                                                             result.assets.reserve(end_index - start_index);
                                                             for (std::size_t idx = start_index; idx < end_index; ++idx) {
                                                                     const auto& item = work_items[idx];
                                                                     try {
                                                                             const bool has_metadata = item.metadata.is_object() && !item.metadata.empty();
                                                                             auto info = AssetInfo::from_manifest_entry(
                                                                                 item.name,
                                                                                 has_metadata ? item.metadata : nlohmann::json::object());
                                                                             result.assets.emplace_back(item.name, std::move(info));
                                                                             ++result.loaded;
                                                                     } catch (const std::exception& error) {
                                                                             ++result.failed;
                                                                             vibble::log::warn(std::string("[AssetLibrary] Failed to load asset '") +
                                                                                               item.name + "': " + error.what());
                                                                     } catch (...) {
                                                                             ++result.failed;
                                                                             vibble::log::warn(std::string("[AssetLibrary] Failed to load asset '") +
                                                                                               item.name + "' due to an unknown error.");
                                                                     }
                                                             }
                                                             return result;
                                                     }));
                }

                for (auto& future : futures) {
                        WorkerResult result = future.get();
                        loaded += result.loaded;
                        failed += result.failed;
                        for (auto& entry : result.assets) {
                                info_by_name_[entry.first] = std::move(entry.second);
                        }
                }
        }
        const auto end_ms = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_ms - start_ms).count();
        vibble::log::info(std::string("[AssetLibrary] Loaded ") + std::to_string(info_by_name_.size()) + " assets (ok=" + std::to_string(loaded) + ", failed=" + std::to_string(failed) + ") in " + std::to_string(elapsed_ms) + "ms");
}

std::vector<std::string> AssetLibrary::sync_missing_from_resources() {
    manifest::ManifestData manifest;
    const auto assets_root = assets_root_path();
    if (!load_manifest_with_asset_sync(manifest, assets_root)) {
        return {};
    }

    std::vector<std::string> added_assets;
    for (auto it = manifest.assets.begin(); it != manifest.assets.end(); ++it) {
        const std::string name = it.key();
        const auto& metadata = it.value();
        if (name.empty() || info_by_name_.find(name) != info_by_name_.end()) {
            continue;
        }
        if (!metadata.is_object()) {
            vibble::log::warn(std::string("[AssetLibrary] Cannot sync missing asset '") + name + "' because its manifest entry is malformed.");
            continue;
        }
        add_asset(name, metadata);
        if (info_by_name_.find(name) != info_by_name_.end()) {
            added_assets.push_back(name);
        }
    }

    if (!added_assets.empty()) {
        vibble::log::info(std::string("[AssetLibrary] Synced missing assets ") + summarize_names(added_assets));
    }
    return added_assets;
}

void AssetLibrary::add_asset(const std::string& name, const nlohmann::json& metadata) {
    if (info_by_name_.count(name)) {

        return;
    }

    try {
        std::shared_ptr<AssetInfo> info = AssetInfo::from_manifest_entry(name, metadata);
        info_by_name_[name] = info;
        runtime_loaded_assets_.erase(name);
        vibble::log::info(std::string("[AssetLibrary] Added asset '") + name + "' to library");
    } catch (const std::exception& error) {
        vibble::log::error(std::string("[AssetLibrary] Failed to add asset '") + name + "': " + error.what());
    } catch (...) {
        vibble::log::error(std::string("[AssetLibrary] Failed to add asset '") + name + "' due to an unknown error.");
    }
}

std::shared_ptr<AssetInfo> AssetLibrary::get(const std::string& name) const {
	auto it = info_by_name_.find(name);
	if (it != info_by_name_.end()) {
		return it->second;
	}
	return nullptr;
}

const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>&
AssetLibrary::all() const {
	return info_by_name_;
}

std::vector<std::string> AssetLibrary::names() const {
        std::vector<std::string> result;
        result.reserve(info_by_name_.size());
        for (const auto& [name, _] : info_by_name_) {
                result.push_back(name);
        }
        std::sort(result.begin(), result.end());
        return result;
}

void AssetLibrary::loadAllAnimations(SDL_Renderer* renderer) {
    if (!renderer) {
        return;
    }

    const auto begin = std::chrono::steady_clock::now();
    std::size_t loaded = 0;
    for (auto& [name, info] : info_by_name_) {
        if (!info) {
            continue;
        }
        info->loadAnimations(renderer);
        runtime_loaded_assets_.insert(name);
        ++loaded;
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    vibble::log::info(std::string("[AssetLibrary] Preloaded animations for ") + std::to_string(loaded) + " asset(s) in " + std::to_string(elapsed_ms) + "ms");
    startup_warmup_complete_ = true;
}

void AssetLibrary::ensureAllAnimationsLoaded(SDL_Renderer* renderer) {
    if (!renderer || startup_warmup_complete_) {
        return;
    }

    std::unordered_set<std::string> all_names;
    all_names.reserve(info_by_name_.size());
    for (const auto& [name, info] : info_by_name_) {
        if (info && !name.empty()) {
            all_names.insert(name);
        }
    }
    const WarmupPassStats stats = warmup_assets(
        renderer,
        info_by_name_,
        runtime_loaded_assets_,
        sorted_names_from_set(all_names),
        "Preload pass 1",
        "Pure load pass");
    startup_warmup_complete_ = stats.load_failed.empty() && stats.cache_failed.empty();
}

void AssetLibrary::ensureAnimationsLoadedFor(SDL_Renderer* renderer, const std::unordered_set<std::string>& names) {
    if (!renderer || names.empty()) {
        return;
    }

    (void)warmup_assets(
        renderer,
        info_by_name_,
        runtime_loaded_assets_,
        sorted_names_from_set(names),
        "Targeted preload",
        "Targeted load");
}

void AssetLibrary::loadAnimationsFor(SDL_Renderer* renderer, const std::unordered_set<std::string>& names) {
    if (!renderer || names.empty()) {
        return;
    }
    vibble::log::debug(std::string("[AssetLibrary] loadAnimationsFor: count=") + std::to_string(names.size()));
    std::size_t idx = 0;
    for (const auto& name : names) {

        vibble::log::debug(std::string("[AssetLibrary] (") + std::to_string(idx) + "/" + std::to_string(names.size()) + ") loading '" + name + "'...");
        auto it = info_by_name_.find(name);
        if (it != info_by_name_.end() && it->second) {
            try {
                const auto load_begin = std::chrono::steady_clock::now();
                // Map startup only needs essential animations immediately (default/start).
                // Missing non-essential animations are hydrated on demand.
                it->second->loadAnimations(renderer, false);
                const auto load_end = std::chrono::steady_clock::now();
                const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_begin).count();
                if (!it->second->animations.empty()) {
                    runtime_loaded_assets_.insert(name);
                }
                if (load_ms > 250) {
                        vibble::log::info(std::string("[AssetLibrary] Preload '") + name + "' took " + std::to_string(load_ms) + "ms");
                }
            } catch (const std::exception& ex) {
                vibble::log::error(std::string("[AssetLibrary] Exception while loading animations for '") + name + "': " + ex.what());
                throw;
            } catch (...) {
                vibble::log::error(std::string("[AssetLibrary] Unknown exception while loading animations for '") + name + "'");
                throw;
            }
        } else {
            vibble::log::warn(std::string("[AssetLibrary] Missing AssetInfo for '") + name + "'");
        }
        ++idx;
    }
}

AssetLibrary::RefreshMissingResult AssetLibrary::repairAndRefreshMissing(SDL_Renderer* renderer) {
    RefreshMissingResult result;
    result.added_assets = sync_missing_from_resources();

    std::vector<AssetInfo*> all_infos;
    all_infos.reserve(info_by_name_.size());
    for (auto& [name, info] : info_by_name_) {
        if (info && !name.empty()) {
            all_infos.push_back(info.get());
        }
    }

    std::unordered_set<std::string> names_to_load(result.added_assets.begin(), result.added_assets.end());
    if (renderer && !all_infos.empty()) {
        PrimaryAssetCache primary_cache(renderer);
        auto detect_result = primary_cache.detect_missing_cache_files(all_infos);
        if (!detect_result.ok) {
            vibble::log::warn(std::string("[AssetLibrary] Repair / Refresh Missing detection failed: ") + detect_result.error);
        } else if (!detect_result.touched_assets.empty()) {
            std::unordered_set<std::string> touched_names(detect_result.touched_assets.begin(), detect_result.touched_assets.end());
            std::vector<AssetInfo*> touched_infos;
            touched_infos.reserve(touched_names.size());
            names_to_load.insert(detect_result.touched_assets.begin(), detect_result.touched_assets.end());
            for (AssetInfo* info : all_infos) {
                if (info && touched_names.find(info->name) != touched_names.end()) {
                    touched_infos.push_back(info);
                }
            }

            auto repair_result = primary_cache.repair_missing_cache_files(touched_infos);
            if (!repair_result.ok) {
                vibble::log::warn(std::string("[AssetLibrary] Repair / Refresh Missing repair failed: ") + repair_result.error);
                append_unique_names(result.failed_assets, detect_result.touched_assets);
            } else {
                for (const auto& [asset_name, written_files] : repair_result.written_files_by_asset) {
                    if (written_files.empty()) {
                        continue;
                    }
                    result.repaired_assets.push_back(asset_name);
                    names_to_load.insert(asset_name);
                }
            }
        }
    } else if (!renderer && !result.added_assets.empty()) {
        vibble::log::warn("[AssetLibrary] Repair / Refresh Missing skipped runtime load because no renderer is available.");
        append_unique_names(result.failed_assets, result.added_assets);
    }

    if (renderer && !names_to_load.empty()) {
        const WarmupPassStats stats = warmup_assets(
            renderer,
            info_by_name_,
            runtime_loaded_assets_,
            sorted_names_from_set(names_to_load),
            "Repair / Refresh Missing preload",
            "Repair / Refresh Missing load");
        append_unique_names(result.repaired_assets, stats.cache_repaired);
        append_unique_names(result.loaded_assets, stats.load_succeeded);
        append_unique_names(result.failed_assets, stats.cache_failed);
        append_unique_names(result.failed_assets, stats.load_failed);
    }

    vibble::log::info(
        std::string("[AssetLibrary] Repair / Refresh Missing summary: added=") +
        std::to_string(result.added_assets.size()) + " " + summarize_names(result.added_assets) +
        ", repaired=" + std::to_string(result.repaired_assets.size()) + " " + summarize_names(result.repaired_assets) +
        ", loaded=" + std::to_string(result.loaded_assets.size()) + " " + summarize_names(result.loaded_assets) +
        ", failed=" + std::to_string(result.failed_assets.size()) + " " + summarize_names(result.failed_assets));
    return result;
}

bool AssetLibrary::remove(const std::string& name) {
    runtime_loaded_assets_.erase(name);
    return info_by_name_.erase(name) > 0;
}

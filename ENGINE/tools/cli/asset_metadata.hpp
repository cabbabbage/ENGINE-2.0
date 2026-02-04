#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "bundle_metadata.hpp"
#include "image_cache_generator.hpp"

namespace imgcache {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

inline std::vector<std::string> DiscoverAssetNames(const fs::path& assets_root) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::exists(assets_root, ec) || ec || !fs::is_directory(assets_root, ec)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(assets_root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

inline ordered_json* AnimationsObject(ordered_json& asset_meta) {
    if (!asset_meta.is_object()) return nullptr;
    if (!asset_meta.contains("animations") || !asset_meta["animations"].is_object()) {
        return nullptr;
    }
    auto& payloads = asset_meta["animations"];
    if (payloads.contains("animations") && payloads["animations"].is_object()) {
        return &payloads["animations"];
    }
    return &payloads;
}

inline const ordered_json* AnimationsObject(const ordered_json& asset_meta) {
    if (!asset_meta.is_object()) return nullptr;
    if (!asset_meta.contains("animations") || !asset_meta["animations"].is_object()) {
        return nullptr;
    }
    const auto& payloads = asset_meta["animations"];
    if (payloads.contains("animations") && payloads["animations"].is_object()) {
        return &payloads["animations"];
    }
    return &payloads;
}

struct AssetRecord {
    std::string name;
    ordered_json meta;
    fs::path source_dir;
    std::unordered_map<std::string, fs::path> discovered_anims;
    std::vector<std::string> anim_names;
};

inline fs::path ResolveAnimDir(const fs::path& asset_src_dir,
                               const std::string& anim_name,
                               const ordered_json& anim_meta,
                               const std::unordered_map<std::string, fs::path>& discovered_anims) {
    if (anim_meta.is_object() && anim_meta.contains("source") && anim_meta["source"].is_object()) {
        const auto& source = anim_meta["source"];
        const std::string kind = source.value("kind", std::string{});
        if (kind == "folder") {
            const std::string path = source.value("path", anim_name);
            if (!path.empty()) {
                fs::path p(path);
                if (p.is_absolute()) {
                    return p;
                }
                return asset_src_dir / p;
            }
        }
    }
    auto it = discovered_anims.find(anim_name);
    if (it != discovered_anims.end()) {
        return it->second;
    }
    if (anim_name == "default") {
        return asset_src_dir;
    }
    return asset_src_dir / anim_name;
}

inline AssetRecord BuildAssetRecord(const fs::path& manifest_dir,
                                    const fs::path& repo_root,
                                    const fs::path& cache_root,
                                    const std::string& asset_name) {
    AssetRecord record;
    record.name = asset_name;

    const fs::path bundle_path = cache_root / asset_name / "bundle.bin";
    auto bundle_meta = LoadBundleMetadataSnapshot(bundle_path);
    if (bundle_meta) {
        record.meta = *bundle_meta;
    } else {
        record.meta = ordered_json::object();
    }

    record.source_dir = ImageCacheGenerator::ResolveAssetSourceDir(manifest_dir, repo_root, asset_name, record.meta);

    const auto discovered = ImageCacheGenerator::DiscoverAnimations(record.source_dir);
    for (const auto& pair : discovered) {
        record.discovered_anims.emplace(pair.first, pair.second);
    }

    const ordered_json* anims = AnimationsObject(record.meta);
    if (anims && anims->is_object()) {
        for (auto it = anims->begin(); it != anims->end(); ++it) {
            if (!it.key().empty()) {
                record.anim_names.push_back(it.key());
            }
        }
    }

    for (const auto& entry : record.discovered_anims) {
        if (std::find(record.anim_names.begin(), record.anim_names.end(), entry.first) == record.anim_names.end()) {
            record.anim_names.push_back(entry.first);
        }
    }

    std::sort(record.anim_names.begin(), record.anim_names.end());
    record.anim_names.erase(std::unique(record.anim_names.begin(), record.anim_names.end()), record.anim_names.end());
    return record;
}

} // namespace imgcache

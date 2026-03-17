#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache_helper.hpp"
#include "image_cache_generator.hpp"

namespace imgcache {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

namespace detail {

constexpr std::uint32_t kBundleVersion = 1;
constexpr std::uint64_t kBundleMagic = 0x424e444c454e4745ull; // "ENGENDLB" little endian folded

#pragma pack(push, 1)
struct BundleHeader {
    std::uint64_t magic = kBundleMagic;
    std::uint32_t version = kBundleVersion;
    std::uint32_t reserved = 0;
    std::uint64_t metadata_size = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t content_hash = 0;
};
#pragma pack(pop)

} // namespace detail

inline std::optional<ordered_json> LoadBundleMetadataSnapshot(const fs::path& bundle_path,
                                                              std::string* error = nullptr) {
    if (error) error->clear();
    std::ifstream in(bundle_path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    detail::BundleHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good()) {
        if (error) *error = "Failed to read bundle header from " + bundle_path.string();
        return std::nullopt;
    }
    if (header.magic != detail::kBundleMagic || header.version != detail::kBundleVersion) {
        if (error) *error = "Bundle magic/version mismatch for " + bundle_path.string();
        return std::nullopt;
    }

    std::string meta_text;
    meta_text.resize(static_cast<std::size_t>(header.metadata_size));
    in.read(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
    if (!in.good()) {
        if (error) *error = "Failed to read bundle metadata from " + bundle_path.string();
        return std::nullopt;
    }

    ordered_json meta;
    try {
        meta = ordered_json::parse(meta_text);
    } catch (const std::exception& ex) {
        if (error) *error = std::string("Failed to parse bundle metadata: ") + ex.what();
        return std::nullopt;
    }

    if (meta.contains("metadata_snapshot") && meta["metadata_snapshot"].is_object()) {
        return meta["metadata_snapshot"];
    }

    return ordered_json::object();
}

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

inline ordered_json BuildImageMetadataSnippet(const ordered_json& asset_meta) {
    ordered_json snippet = ordered_json::object();
    if (!asset_meta.is_object()) {
        return snippet;
    }

    if (asset_meta.contains("size_settings")) {
        snippet["size_settings"] = asset_meta["size_settings"];
    }

    const ordered_json* anims = AnimationsObject(asset_meta);
    if (anims && anims->is_object()) {
        snippet["animations"] = *anims;
    }

    return snippet;
}

inline std::string BuildImageMetadataHash(const ordered_json& asset_meta) {
    return CacheHelper::StableHashHex_BLAKE2b16(BuildImageMetadataSnippet(asset_meta));
}

inline ordered_json LoadAssetMetadataCache(const fs::path& cache_root) {
    const fs::path cache_path = cache_root / "asset_metadata_cache.json";
    auto result = CacheHelper::LoadJsonFile(cache_path.string());
    if (result.ok && result.value.is_object()) {
        ordered_json cache = result.value;
        if (!cache.contains("assets") || !cache["assets"].is_object()) {
            cache["assets"] = ordered_json::object();
        }
        return cache;
    }

    ordered_json cache = ordered_json::object();
    cache["assets"] = ordered_json::object();
    return cache;
}

inline bool SaveAssetMetadataCache(const fs::path& cache_root, const ordered_json& cache) {
    const fs::path cache_path = cache_root / "asset_metadata_cache.json";
    auto result = CacheHelper::WriteJsonFile(cache_path.string(), nlohmann::json(cache));
    return result.ok;
}

inline std::string CachedImageMetadataHash(const ordered_json& cache, const std::string& asset_name) {
    if (!cache.is_object() || !cache.contains("assets") || !cache["assets"].is_object()) {
        return {};
    }
    const auto& assets = cache["assets"];
    auto it = assets.find(asset_name);
    if (it == assets.end() || !it->is_object()) {
        return {};
    }
    const auto& entry = *it;
    if (entry.contains("image_meta_hash") && entry["image_meta_hash"].is_string()) {
        return entry["image_meta_hash"].get<std::string>();
    }
    return {};
}

inline void UpdateAssetMetadataCache(ordered_json& cache,
                                     const std::string& asset_name,
                                     const std::string& hash) {
    if (!cache.is_object()) {
        cache = ordered_json::object();
    }
    if (!cache.contains("assets") || !cache["assets"].is_object()) {
        cache["assets"] = ordered_json::object();
    }
    cache["assets"][asset_name] = ordered_json::object({{"image_meta_hash", hash}});
}

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

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

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

} // namespace imgcache

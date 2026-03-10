#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/manifest/manifest_loader.hpp"

namespace manifest {

struct MapManifestNormalizationResult {
    nlohmann::json map_manifest;
    std::filesystem::path resolved_content_root;
    bool changed = false;
};

struct MapManifestBootstrapResult {
    nlohmann::json map_manifest;
    std::filesystem::path resolved_content_root;
    bool manifest_entry_found = false;
    bool changed = false;
};

MapManifestNormalizationResult normalize_map_manifest(nlohmann::json map_manifest,
                                                      const std::string& map_id,
                                                      const std::filesystem::path& manifest_root);

nlohmann::json build_default_map_manifest(const std::string& map_name);

MapManifestBootstrapResult bootstrap_map_manifest(const ManifestData& manifest_data,
                                                  const std::string& map_id,
                                                  const nlohmann::json* fallback_manifest = nullptr);

}

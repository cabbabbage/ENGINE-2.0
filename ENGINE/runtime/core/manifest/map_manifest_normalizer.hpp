#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace manifest {

struct MapManifestNormalizationResult {
    nlohmann::json map_manifest;
    std::filesystem::path resolved_content_root;
    bool changed = false;
};

MapManifestNormalizationResult normalize_map_manifest(nlohmann::json map_manifest,
                                                      const std::string& map_id,
                                                      const std::filesystem::path& manifest_root);

}

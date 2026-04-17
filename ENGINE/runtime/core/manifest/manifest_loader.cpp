#include "core/manifest/manifest_loader.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace manifest {
namespace {
constexpr int kManifestVersion = 2;

std::filesystem::path project_root() {
#ifdef PROJECT_ROOT
    return std::filesystem::path(PROJECT_ROOT);
#else
    return std::filesystem::current_path();
#endif
}

nlohmann::json make_default_manifest_json() {
    nlohmann::json manifest_json = nlohmann::json::object();
    manifest_json["version"] = kManifestVersion;
    manifest_json["assets"] = nlohmann::json::object();
    manifest_json["maps"] = nlohmann::json::object();
    return manifest_json;
}

ManifestData make_manifest_data(nlohmann::json manifest_json) {
    ManifestData data;
    data.raw = std::move(manifest_json);
    data.assets = data.raw.at("assets");
    data.maps = data.raw.at("maps");
    return data;
}

void ensure_directory_exists(const std::filesystem::path& dir,
                             const char* description) {
    if (dir.empty()) {
        return;
    }

    std::error_code ec;
    if (std::filesystem::create_directories(dir, ec)) {
        return;
    }

    if (ec && !std::filesystem::exists(dir)) {
        std::ostringstream oss;
        oss << "Failed to create " << description << " directory '"
            << dir.u8string() << "': " << ec.message();
        throw std::runtime_error(oss.str());
    }
}

void ensure_project_structure(const std::filesystem::path& root) {
    ensure_directory_exists(root / "resources", "resources root");
    ensure_directory_exists(root / "resources" / "assets", "resources assets");
    ensure_directory_exists(root / "resources" / "misc_content", "resources misc content");
    ensure_directory_exists(root / "resources" / "loading_screen_content", "resources loading screen content");
    ensure_directory_exists(root / "resources" / "LOADING CONTENT", "resources loading content");
}

void write_manifest_file(const std::filesystem::path& path,
                         const nlohmann::json& manifest_json) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        ensure_directory_exists(parent, "manifest parent");
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        std::ostringstream oss;
        oss << "Unable to open manifest file at '" << path.string() << "' for writing.";
        throw std::runtime_error(oss.str());
    }
    out << manifest_json.dump(2);
    if (!out.good()) {
        std::ostringstream oss;
        oss << "Failed while writing manifest file at '" << path.string() << "'.";
        throw std::runtime_error(oss.str());
    }
}

std::string manifest_validation_error(const std::filesystem::path& path,
                                      const char* message) {
    std::ostringstream oss;
    oss << "manifest: '" << path.string() << "' " << message;
    return oss.str();
}

void validate_manifest_json(const nlohmann::json& manifest_json,
                            const std::filesystem::path& path) {
    if (!manifest_json.is_object()) {
        throw std::runtime_error(manifest_validation_error(path, "must be an object."));
    }

    auto version_it = manifest_json.find("version");
    if (version_it == manifest_json.end() || !version_it->is_number_integer()) {
        throw std::runtime_error(manifest_validation_error(path, "\"version\" must be an integer."));
    }

    const int version_value = version_it->get<int>();
    if (version_value != kManifestVersion) {
        std::ostringstream oss;
        oss << "manifest: '" << path.string() << "' has version " << version_value
            << " but expected " << kManifestVersion << ".";
        throw std::runtime_error(oss.str());
    }

    auto assets_it = manifest_json.find("assets");
    if (assets_it == manifest_json.end() || !assets_it->is_object()) {
        throw std::runtime_error(manifest_validation_error(path, "\"assets\" must be an object."));
    }

    auto maps_it = manifest_json.find("maps");
    if (maps_it == manifest_json.end() || !maps_it->is_object()) {
        throw std::runtime_error(manifest_validation_error(path, "\"maps\" must be an object."));
    }
}

void validate_manifest_grid_settings_schema(const nlohmann::json& manifest_json,
                                            const std::filesystem::path& path) {
    auto maps_it = manifest_json.find("maps");
    if (maps_it == manifest_json.end() || !maps_it->is_object()) {
        return;
    }

    for (auto it = maps_it->begin(); it != maps_it->end(); ++it) {
        if (!it.value().is_object()) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' map entry '" << it.key()
                << "' must be an object.";
            throw std::runtime_error(oss.str());
        }

        auto settings_it = it.value().find("map_grid_settings");
        if (settings_it == it.value().end() || !settings_it->is_object()) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' map entry '" << it.key()
                << "' must contain object field \"map_grid_settings\".";
            throw std::runtime_error(oss.str());
        }

        const auto grid_resolution_it = settings_it->find("grid_resolution");
        if (grid_resolution_it == settings_it->end() || !grid_resolution_it->is_number_integer()) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' map entry '" << it.key()
                << "' must define integer \"map_grid_settings.grid_resolution\".";
            throw std::runtime_error(oss.str());
        }

        const std::array<const char*, 7> legacy_keys{
            "resolution",
            "spacing",
            "jitter",
            "r_chunk",
            "chunk_resolution",
            "chunk_size",
            "chunk_size_px"
        };
        for (const char* legacy_key : legacy_keys) {
            if (settings_it->contains(legacy_key)) {
                std::ostringstream oss;
                oss << "manifest: '" << path.string() << "' map entry '" << it.key()
                    << "' still uses deprecated key \"map_grid_settings." << legacy_key
                    << "\". Run manifest migration tooling before launching runtime.";
                throw std::runtime_error(oss.str());
            }
        }
    }
}

void validate_manifest_asset_schema(const nlohmann::json& manifest_json,
                                    const std::filesystem::path& path) {
    auto assets_it = manifest_json.find("assets");
    if (assets_it == manifest_json.end() || !assets_it->is_object()) {
        return;
    }

    for (auto it = assets_it->begin(); it != assets_it->end(); ++it) {
        if (!it.value().is_object()) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                << "' must be an object.";
            throw std::runtime_error(oss.str());
        }

        const auto& asset = it.value();
        const char* bool_keys[] = {
            "movement_enabled",
            "attack_box_enabled",
            "hitbox_enabled",
            "floor_boxes_enabled",
        };
        for (const char* key : bool_keys) {
            auto flag_it = asset.find(key);
            if (flag_it != asset.end() && !flag_it->is_boolean()) {
                std::ostringstream oss;
                oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                    << "' key '" << key << "' must be boolean when present.";
                throw std::runtime_error(oss.str());
            }
        }

        auto floor_boxes_it = asset.find("floor_boxes");
        const bool has_floor_boxes_enabled_flag =
            asset.contains("floor_boxes_enabled") && asset["floor_boxes_enabled"].is_boolean();
        const bool floor_boxes_enabled = has_floor_boxes_enabled_flag
            ? asset["floor_boxes_enabled"].get<bool>()
            : (floor_boxes_it != asset.end() && floor_boxes_it->is_array() && !floor_boxes_it->empty());
        if (has_floor_boxes_enabled_flag && !floor_boxes_enabled) {
            if (floor_boxes_it != asset.end()) {
                std::ostringstream oss;
                oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                    << "' must omit 'floor_boxes' when 'floor_boxes_enabled' is false.";
                throw std::runtime_error(oss.str());
            }
            continue;
        }

        if (floor_boxes_it == asset.end()) {
            continue;
        }
        if (!floor_boxes_it->is_array()) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                << "' key 'floor_boxes' must be an array.";
            throw std::runtime_error(oss.str());
        }

        std::size_t boundary_count = 0;
        for (const auto& floor_box : *floor_boxes_it) {
            if (!floor_box.is_object()) {
                std::ostringstream oss;
                oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                    << "' contains non-object floor box entry.";
                throw std::runtime_error(oss.str());
            }
            const char* string_keys[] = {"id", "name"};
            for (const char* key : string_keys) {
                if (!floor_box.contains(key) || !floor_box[key].is_string()) {
                    std::ostringstream oss;
                    oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                        << "' floor box key '" << key << "' must be a string.";
                    throw std::runtime_error(oss.str());
                }
            }
            const char* number_keys[] = {"position_x", "position_z", "width", "depth", "rotation_degrees"};
            for (const char* key : number_keys) {
                if (!floor_box.contains(key) || !floor_box[key].is_number()) {
                    std::ostringstream oss;
                    oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                        << "' floor box key '" << key << "' must be numeric.";
                    throw std::runtime_error(oss.str());
                }
            }
            if (!floor_box.contains("is_boundary") || !floor_box["is_boundary"].is_boolean() ||
                !floor_box.contains("enabled") || !floor_box["enabled"].is_boolean()) {
                std::ostringstream oss;
                oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                    << "' floor box keys 'is_boundary' and 'enabled' must be booleans.";
                throw std::runtime_error(oss.str());
            }
            if (floor_box.value("is_boundary", false)) {
                ++boundary_count;
            }
        }
        if (boundary_count > 1) {
            std::ostringstream oss;
            oss << "manifest: '" << path.string() << "' asset entry '" << it.key()
                << "' has " << boundary_count
                << " boundary floor boxes; at most one is allowed.";
            throw std::runtime_error(oss.str());
        }
    }
}

} // namespace

std::string manifest_path() {
    return (project_root() / "manifest.json").string();
}

ManifestData load_manifest() {
    const std::filesystem::path root = project_root();
    const std::filesystem::path path = root / "manifest.json";

    ensure_project_structure(root);

    if (!std::filesystem::exists(path)) {
        auto data = make_manifest_data(make_default_manifest_json());
        write_manifest_file(path, data.raw);
        return data;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        std::ostringstream oss;
        oss << "Unable to open manifest file at '" << path.string() << "' for reading.";
        throw std::runtime_error(oss.str());
    }

    nlohmann::json manifest_json;
    try {
        in >> manifest_json;
    } catch (const nlohmann::json::parse_error& error) {
        std::ostringstream oss;
        oss << "Failed to parse manifest at '" << path.string() << "': " << error.what();
        throw std::runtime_error(oss.str());
    }

    validate_manifest_json(manifest_json, path);
    validate_manifest_grid_settings_schema(manifest_json, path);
    validate_manifest_asset_schema(manifest_json, path);
    return make_manifest_data(std::move(manifest_json));
}

void save_manifest(const ManifestData& data) {
    const std::filesystem::path root = project_root();
    const std::filesystem::path path = root / "manifest.json";
    write_manifest_file(path, data.raw);
}

} // namespace manifest

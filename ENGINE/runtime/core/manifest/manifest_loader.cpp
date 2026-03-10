#include "core/manifest/manifest_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "utils/grid.hpp"

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

int clamp_grid_resolution(int value) {
    return std::clamp(value, 0, vibble::grid::kMaxResolution);
}

int legacy_resolution_from_spacing(int spacing) {
    const int clamped = std::max(1, spacing);
    const double log_value = std::log(static_cast<double>(clamped)) / std::log(3.0);
    return clamp_grid_resolution(static_cast<int>(std::lround(log_value)));
}

int legacy_resolution_from_chunk_size(int chunk_size) {
    const int clamped = std::max(1, chunk_size);
    const double log_value = std::log2(static_cast<double>(clamped));
    return clamp_grid_resolution(static_cast<int>(std::lround(log_value)));
}

bool migrate_single_map_grid_settings(nlohmann::json& map_entry) {
    if (!map_entry.is_object()) {
        map_entry = nlohmann::json::object();
        map_entry["map_grid_settings"] = nlohmann::json::object({{"grid_resolution", 0}});
        return true;
    }

    nlohmann::json& section = map_entry["map_grid_settings"];
    if (!section.is_object()) {
        section = nlohmann::json::object();
    }

    bool changed = false;
    int resolved_resolution = 0;
    bool resolved = false;

    auto read_legacy_resolution = [&](const char* key) {
        if (!section.contains(key) || !section[key].is_number_integer()) {
            return false;
        }
        resolved_resolution = clamp_grid_resolution(section[key].get<int>());
        resolved = true;
        return true;
    };

    if (section.contains("grid_resolution") && section["grid_resolution"].is_number_integer()) {
        resolved_resolution = clamp_grid_resolution(section["grid_resolution"].get<int>());
        resolved = true;
    }

    if (!resolved) {
        (void)read_legacy_resolution("resolution");
    }
    if (!resolved && section.contains("spacing") && section["spacing"].is_number_integer()) {
        resolved_resolution = legacy_resolution_from_spacing(section["spacing"].get<int>());
        resolved = true;
    }
    if (!resolved) {
        (void)read_legacy_resolution("r_chunk");
    }
    if (!resolved) {
        (void)read_legacy_resolution("chunk_resolution");
    }
    if (!resolved && section.contains("chunk_size") && section["chunk_size"].is_number_integer()) {
        resolved_resolution = legacy_resolution_from_chunk_size(section["chunk_size"].get<int>());
        resolved = true;
    }
    if (!resolved && section.contains("chunk_size_px") && section["chunk_size_px"].is_number_integer()) {
        resolved_resolution = legacy_resolution_from_chunk_size(section["chunk_size_px"].get<int>());
        resolved = true;
    }

    if (!resolved) {
        resolved_resolution = 0;
    }

    if (!section.contains("grid_resolution") ||
        !section["grid_resolution"].is_number_integer() ||
        section["grid_resolution"].get<int>() != resolved_resolution) {
        section["grid_resolution"] = resolved_resolution;
        changed = true;
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
    for (const char* key : legacy_keys) {
        if (section.erase(key) > 0) {
            changed = true;
        }
    }

    return changed;
}

bool migrate_manifest_grid_settings(nlohmann::json& manifest_json) {
    auto maps_it = manifest_json.find("maps");
    if (maps_it == manifest_json.end() || !maps_it->is_object()) {
        return false;
    }

    bool changed = false;
    for (auto it = maps_it->begin(); it != maps_it->end(); ++it) {
        if (migrate_single_map_grid_settings(it.value())) {
            changed = true;
        }
    }
    return changed;
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

    if (migrate_manifest_grid_settings(manifest_json)) {
        write_manifest_file(path, manifest_json);
    }
    return make_manifest_data(std::move(manifest_json));
}

void save_manifest(const ManifestData& data) {
    const std::filesystem::path root = project_root();
    const std::filesystem::path path = root / "manifest.json";
    write_manifest_file(path, data.raw);
}

} // namespace manifest

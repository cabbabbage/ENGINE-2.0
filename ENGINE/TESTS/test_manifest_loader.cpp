#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "core/manifest/manifest_loader.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/manifest_asset_utils.hpp"
#include "utils/log.hpp"

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

static fs::path test_root() {
#ifdef PROJECT_ROOT
    return fs::path(PROJECT_ROOT);
#else
    return fs::current_path() / "TEST_TMP";
#endif
}

TEST_CASE("manifest loader enforces the current schema exactly") {
    vibble::log::set_level(vibble::log::Level::Warn);

    const fs::path root = test_root();
    const fs::path manifest = root / "manifest.json";
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::remove(manifest, ec);

    auto data1 = manifest::load_manifest();
    REQUIRE(data1.raw.is_object());
    REQUIRE(data1.raw["version"].is_number_integer());
    CHECK(data1.raw["version"].get<int>() == 1);

    nlohmann::json canonical = nlohmann::json::object();
    canonical["version"] = 1;
    canonical["assets"] = nlohmann::json::object();
    canonical["maps"] = nlohmann::json::object();

    auto write_manifest = [&](const nlohmann::json& payload) {
        std::ofstream out(manifest);
        REQUIRE(out.is_open());
        out << payload.dump(2);
    };

    {
        std::ofstream out(manifest);
        REQUIRE(out.is_open());
        out << "{\n";
    }
    CHECK_THROWS_AS(manifest::load_manifest(), std::runtime_error);

    write_manifest(canonical);

    nlohmann::json wrong_version = canonical;
    wrong_version["version"] = 2;
    write_manifest(wrong_version);
    CHECK_THROWS_AS(manifest::load_manifest(), std::runtime_error);

    write_manifest(canonical);
    nlohmann::json missing_assets = canonical;
    missing_assets.erase("assets");
    write_manifest(missing_assets);
    CHECK_THROWS_AS(manifest::load_manifest(), std::runtime_error);

    write_manifest(canonical);
    nlohmann::json missing_maps = canonical;
    missing_maps.erase("maps");
    write_manifest(missing_maps);
    CHECK_THROWS_AS(manifest::load_manifest(), std::runtime_error);

    write_manifest(canonical);
    auto data2 = manifest::load_manifest();
    CHECK(data2.raw["version"].get<int>() == 1);
}

TEST_CASE("manifest store helper removes asset entries") {
    vibble::log::set_level(vibble::log::Level::Warn);

    const fs::path root = test_root() / "manifest_remove_helper";
    const fs::path manifest_path = root / "manifest.json";
    std::error_code ec;
    fs::create_directories(root, ec);

    nlohmann::json initial = nlohmann::json::object();
    initial["version"] = 1;
    initial["assets"] = {
        {"Alpha", {
            {"asset_name", "Alpha"},
        {"asset_directory", "resources/assets/Alpha"},
            {"asset_type", "Object"}
        }}
    };
    initial["maps"] = nlohmann::json::object();

    {
        std::ofstream out(manifest_path);
        REQUIRE(out.is_open());
        out << initial.dump(2);
    }

    auto loader = [&]() {
        manifest::ManifestData data;
        std::ifstream in(manifest_path);
        REQUIRE(in.is_open());
        in >> data.raw;
        if (!data.raw.is_object()) {
            data.raw = nlohmann::json::object();
        }
        if (!data.raw.contains("assets")) {
            data.raw["assets"] = nlohmann::json::object();
        }
        if (!data.raw.contains("maps")) {
            data.raw["maps"] = nlohmann::json::object();
        }
        data.assets = data.raw["assets"];
        data.maps = data.raw["maps"];
        return data;
    };

    auto submit = [&](const fs::path&, const nlohmann::json& payload, int indent) {
        std::ofstream out(manifest_path);
        REQUIRE(out.is_open());
        out << payload.dump(indent);
    };

    bool flushed = false;
    auto flush = [&]() { flushed = true; };

    devmode::core::ManifestStore store(manifest_path, loader, submit, flush, 2);
    REQUIRE(store.resolve_asset_name("Alpha").has_value());

    const auto result = devmode::manifest_utils::remove_asset_entry(&store, "Alpha");
    CHECK(result.removed);
    CHECK(result.used_store);
    CHECK_FALSE(store.resolve_asset_name("Alpha").has_value());
    CHECK(store.dirty());

    store.flush();
    CHECK(flushed);

    nlohmann::json written;
    {
        std::ifstream in(manifest_path);
        REQUIRE(in.is_open());
        in >> written;
    }
    CHECK(written["assets"].is_object());
    CHECK(written["assets"].empty());
}

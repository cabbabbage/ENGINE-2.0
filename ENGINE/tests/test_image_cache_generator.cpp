#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "image_cache_generator.hpp"

namespace {

namespace fs = std::filesystem;

class CapturingLogger final : public imgcache::ILogger {
public:
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    void info(const std::string& msg) override { infos.push_back(msg); }
    void warn(const std::string& msg) override { warnings.push_back(msg); }
    void error(const std::string& msg) override { errors.push_back(msg); }
};

fs::path make_unique_temp_dir(const std::string& suffix) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("engine_image_cache_generator_" + suffix + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    ec.clear();
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    return dir;
}

imgcache::ImageRGBA make_rgba(int w, int h) {
    imgcache::ImageRGBA image;
    image.w = w;
    image.h = h;
    image.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0u);
    return image;
}

void fill_alpha_rect(imgcache::ImageRGBA& image, int left, int top, int right, int bottom) {
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) +
                                     static_cast<std::size_t>(x)) * 4u;
            image.pixels[idx + 0] = 255u;
            image.pixels[idx + 1] = 255u;
            image.pixels[idx + 2] = 255u;
            image.pixels[idx + 3] = 255u;
        }
    }
}

void write_png(const fs::path& path, const imgcache::ImageRGBA& image) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    REQUIRE_FALSE(ec);

    std::string err;
    REQUIRE(imgcache::ImageCacheGenerator::SavePngRGBA(path, image, err));
    REQUIRE(err.empty());
}

void write_manifest(const fs::path& repo_root, const std::vector<std::string>& asset_names) {
    nlohmann::json assets = nlohmann::json::object();
    for (const std::string& asset_name : asset_names) {
        assets[asset_name] = nlohmann::json::object();
    }

    std::ofstream out(repo_root / "manifest.json", std::ios::binary);
    REQUIRE(out.is_open());
    out << nlohmann::json{{"assets", assets}}.dump(2);
}

std::vector<int> cache_scale_percents() {
    return {100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
}

} // namespace

TEST_CASE("ImageCacheGenerator keeps scaled shared crop canvas large enough for rounded alpha bounds") {
    const fs::path repo_root = make_unique_temp_dir("rounded_alpha_bounds");
    const fs::path cache_root = repo_root / "cache_out";
    const std::vector<std::string> asset_names{"rounding_asset", "other_repair_candidate"};
    write_manifest(repo_root, asset_names);

    imgcache::ImageRGBA rounding_frame = make_rgba(3, 3);
    fill_alpha_rect(rounding_frame, 0, 0, 2, 2);
    write_png(repo_root / "resources" / "assets" / "rounding_asset" / "default" / "0.png", rounding_frame);

    imgcache::ImageRGBA other_frame = make_rgba(4, 4);
    fill_alpha_rect(other_frame, 1, 1, 3, 3);
    write_png(repo_root / "resources" / "assets" / "other_repair_candidate" / "default" / "0.png", other_frame);

    imgcache::GeneratorOptions options;
    options.manifest_path = repo_root / "manifest.json";
    options.cache_root_override = cache_root;
    options.force_rebuild = true;
    options.filters.assets.insert("rounding_asset");
    options.filters.assets.insert("other_repair_candidate");

    CapturingLogger logger;
    const imgcache::GenResult result = imgcache::ImageCacheGenerator::Run(options, logger);

    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(result.stats.tasks_failed == 0);
    CHECK(result.stats.pngs_written == 20);
    CHECK(result.written_files.size() == 20);
    CHECK(std::find(result.touched_assets.begin(), result.touched_assets.end(), "rounding_asset") !=
          result.touched_assets.end());
    CHECK(std::find(result.touched_assets.begin(), result.touched_assets.end(), "other_repair_candidate") !=
          result.touched_assets.end());

    for (const std::string& asset_name : asset_names) {
        for (int pct : cache_scale_percents()) {
            const fs::path expected = imgcache::CachePaths::frame_png_path(cache_root,
                                                                           asset_name,
                                                                           "default",
                                                                           pct,
                                                                           imgcache::Variant::Normal,
                                                                           0);
            CHECK_MESSAGE(fs::exists(expected), "expected generated cache PNG: " << expected.string());
        }
    }

    std::error_code ec;
    fs::remove_all(repo_root, ec);
}

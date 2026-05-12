#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "rendering/render/shader_package_library.hpp"

namespace {

std::filesystem::path unique_temp_dir(const char* suffix) {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "vibble_shader_pkg_tests";
    const auto stamp = std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return base / (std::string(suffix) + "_" + stamp);
}

void write_binary(const std::filesystem::path& path, const std::uint8_t* bytes, std::size_t count) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(count));
}

} // namespace

TEST_CASE("ShaderPackageLibrary loads manifest with validated SPIR-V payloads") {
    const std::filesystem::path temp_root = unique_temp_dir("load_ok");
    const std::filesystem::path spirv_path = temp_root / "spirv" / "floor_compose.spv";
    const std::filesystem::path manifest_path = temp_root / "runtime_shaders.json";

    const std::array<std::uint8_t, 8> spirv_payload = {
        0x03, 0x02, 0x23, 0x07, 0, 0, 0, 0};

    write_binary(spirv_path, spirv_payload.data(), spirv_payload.size());

    const std::string manifest = R"json(
{
  "manifest_version": 2,
  "variants": {
    "floor_compose": {
      "spirv": {
        "path": "spirv/floor_compose.spv",
        "entrypoint": "main",
        "stage": "fragment",
        "file_size_bytes": 8
      }
    }
  }
}
)json";
    std::filesystem::create_directories(temp_root);
    std::ofstream manifest_out(manifest_path);
    REQUIRE(manifest_out.is_open());
    manifest_out << manifest;
    manifest_out.close();

    ShaderPackageLibrary library;
    std::string error;
    CHECK(library.load_from_manifest(manifest_path, error));
    CHECK(error.empty());
    CHECK(library.manifest_version() == 2);
    CHECK(library.variant_count() == 1);
    CHECK(library.find("floor_compose") != nullptr);

    std::error_code cleanup_error;
    std::filesystem::remove_all(temp_root, cleanup_error);
}

TEST_CASE("ShaderPackageLibrary fails loudly when a referenced payload is missing") {
    const std::filesystem::path temp_root = unique_temp_dir("load_missing");
    const std::filesystem::path manifest_path = temp_root / "runtime_shaders.json";

    const std::string manifest = R"json(
{
  "manifest_version": 2,
  "variants": {
    "sprite_textured": {
      "spirv": {
        "path": "spirv/sprite_textured.spv",
        "entrypoint": "main",
        "stage": "fragment"
      }
    }
  }
}
)json";
    std::ofstream manifest_out(manifest_path);
    REQUIRE(manifest_out.is_open());
    manifest_out << manifest;
    manifest_out.close();

    ShaderPackageLibrary library;
    std::string error;
    CHECK_FALSE(library.load_from_manifest(manifest_path, error));
    CHECK_FALSE(error.empty());
    CHECK(error.find("Unable to open shader binary") != std::string::npos);

    std::error_code cleanup_error;
    std::filesystem::remove_all(temp_root, cleanup_error);
}

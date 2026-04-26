#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ShaderPackageLibrary {
public:
    struct ShaderBinaryDescriptor {
        std::filesystem::path path{};
        std::string entrypoint = "main";
        std::string stage = "auto";
        std::uint64_t hash_fnv1a64 = 0;
        std::uint64_t file_size_bytes = 0;
        std::vector<std::uint8_t> payload{};
        bool available = false;
    };

    struct ShaderVariantPath {
        ShaderBinaryDescriptor dxil{};
        ShaderBinaryDescriptor spirv{};
    };

    bool load_from_manifest(const std::filesystem::path& manifest_path, std::string& out_error);
    const ShaderVariantPath* find(const std::string& shader_name) const;
    std::size_t variant_count() const { return variants_.size(); }
    int manifest_version() const { return manifest_version_; }

private:
    int manifest_version_ = 0;
    std::unordered_map<std::string, ShaderVariantPath> variants_;
};

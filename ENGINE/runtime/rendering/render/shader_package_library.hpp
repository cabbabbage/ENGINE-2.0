#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

class ShaderPackageLibrary {
public:
    struct ShaderVariantPath {
        std::filesystem::path dxil;
        std::filesystem::path spirv;
    };

    bool load_from_manifest(const std::filesystem::path& manifest_path, std::string& out_error);
    const ShaderVariantPath* find(const std::string& shader_name) const;

private:
    std::unordered_map<std::string, ShaderVariantPath> variants_;
};

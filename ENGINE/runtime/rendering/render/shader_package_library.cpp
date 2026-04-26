#include "rendering/render/shader_package_library.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

bool ShaderPackageLibrary::load_from_manifest(const std::filesystem::path& manifest_path, std::string& out_error) {
    variants_.clear();

    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        out_error = "Unable to open shader manifest: " + manifest_path.string();
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const std::exception& ex) {
        out_error = "Invalid shader manifest JSON: " + std::string(ex.what());
        return false;
    }

    const auto variants_it = root.find("variants");
    if (variants_it == root.end() || !variants_it->is_object()) {
        out_error = "Shader manifest is missing 'variants' object";
        return false;
    }

    const std::filesystem::path base_dir = manifest_path.parent_path();
    for (auto it = variants_it->begin(); it != variants_it->end(); ++it) {
        if (!it.value().is_object()) {
            out_error = "Shader variant entry must be an object: " + it.key();
            return false;
        }

        const std::string dxil_rel = it.value().value("dxil", std::string{});
        const std::string spirv_rel = it.value().value("spirv", std::string{});
        if (dxil_rel.empty() && spirv_rel.empty()) {
            out_error = "Shader variant must provide dxil or spirv path: " + it.key();
            return false;
        }

        ShaderVariantPath variant{};
        if (!dxil_rel.empty()) {
            variant.dxil = base_dir / dxil_rel;
        }
        if (!spirv_rel.empty()) {
            variant.spirv = base_dir / spirv_rel;
        }
        variants_.insert_or_assign(it.key(), std::move(variant));
    }

    out_error.clear();
    return true;
}

const ShaderPackageLibrary::ShaderVariantPath* ShaderPackageLibrary::find(const std::string& shader_name) const {
    const auto it = variants_.find(shader_name);
    if (it == variants_.end()) {
        return nullptr;
    }
    return &it->second;
}

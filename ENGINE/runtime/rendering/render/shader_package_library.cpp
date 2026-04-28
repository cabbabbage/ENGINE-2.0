#include "rendering/render/shader_package_library.hpp"

#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

std::uint64_t fnv1a64_bytes(const std::vector<std::uint8_t>& bytes) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffsetBasis;
    for (const std::uint8_t b : bytes) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= kPrime;
    }
    return hash;
}

std::string lowercase_ascii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool is_valid_stage_name(const std::string& stage_name) {
    const std::string lowered = lowercase_ascii(stage_name);
    return lowered == "auto" ||
           lowered == "vertex" ||
           lowered == "fragment" ||
           lowered == "compute";
}

bool parse_u64_hex_or_dec(const nlohmann::json& value, std::uint64_t& out_value) {
    out_value = 0;
    if (value.is_number_unsigned()) {
        out_value = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_string()) {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t idx = 0;
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
        }
        out_value = std::stoull(text, &idx, base);
        return idx == text.size();
    } catch (...) {
        return false;
    }
}

bool read_binary_file(const std::filesystem::path& path,
                      std::vector<std::uint8_t>& out_bytes,
                      std::string& out_error) {
    out_bytes.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        out_error = "Unable to open shader binary: " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        out_error = "Shader binary is empty: " + path.string();
        return false;
    }
    in.seekg(0, std::ios::beg);
    out_bytes.resize(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(out_bytes.data()), size)) {
        out_error = "Failed to read shader binary: " + path.string();
        return false;
    }
    out_error.clear();
    return true;
}

bool validate_binary_magic(const std::filesystem::path& path,
                           std::string_view backend,
                           const std::vector<std::uint8_t>& bytes,
                           std::string& out_error) {
    if (bytes.empty()) {
        out_error = "Shader binary is empty: " + path.string();
        return false;
    }

    const std::string extension = lowercase_ascii(path.extension().string());
    if (backend == "spirv" || extension == ".spv") {
        if (bytes.size() < 4) {
            out_error = "SPIR-V shader is too small: " + path.string();
            return false;
        }
        const std::uint32_t magic =
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[3]) << 24u);
        if (magic != 0x07230203u) {
            out_error = "SPIR-V magic mismatch for: " + path.string();
            return false;
        }
        return true;
    }

    if (backend == "dxil" || extension == ".dxil") {
        if (bytes.size() < 4) {
            out_error = "DXIL shader is too small: " + path.string();
            return false;
        }
        const bool has_dxbc_magic =
            bytes[0] == static_cast<std::uint8_t>('D') &&
            bytes[1] == static_cast<std::uint8_t>('X') &&
            bytes[2] == static_cast<std::uint8_t>('B') &&
            bytes[3] == static_cast<std::uint8_t>('C');
        if (!has_dxbc_magic) {
            out_error = "DXIL container header mismatch for: " + path.string();
            return false;
        }
        return true;
    }

    return true;
}

bool parse_binary_descriptor(const nlohmann::json& raw,
                             const std::filesystem::path& base_dir,
                             std::string_view backend,
                             ShaderPackageLibrary::ShaderBinaryDescriptor& out_desc,
                             std::string& out_error) {
    out_desc = ShaderPackageLibrary::ShaderBinaryDescriptor{};

    std::string rel_path;
    if (raw.is_string()) {
        rel_path = raw.get<std::string>();
    } else if (raw.is_object()) {
        rel_path = raw.value("path", std::string{});
        out_desc.entrypoint = raw.value("entrypoint", std::string("main"));
        out_desc.stage = raw.value("stage", std::string("auto"));
        out_desc.stage = lowercase_ascii(out_desc.stage);
        if (!is_valid_stage_name(out_desc.stage)) {
            out_error = "Invalid shader stage '" + out_desc.stage +
                        "' (expected auto/vertex/fragment/compute)";
            return false;
        }
        std::uint64_t expected_size = 0;
        const bool has_expected_size =
            raw.contains("file_size_bytes") &&
            parse_u64_hex_or_dec(raw["file_size_bytes"], expected_size);
        if (raw.contains("file_size_bytes") && !has_expected_size) {
            out_error = "Invalid file_size_bytes value for " + std::string(backend) + " descriptor";
            return false;
        }
        std::uint64_t expected_hash = 0;
        if (raw.contains("hash_fnv1a64") && !parse_u64_hex_or_dec(raw["hash_fnv1a64"], expected_hash)) {
            out_error = "Invalid hash_fnv1a64 value for " + std::string(backend) + " descriptor";
            return false;
        }
        out_desc.hash_fnv1a64 = expected_hash;
        out_desc.file_size_bytes = has_expected_size ? expected_size : 0;
    } else {
        out_error = "Shader descriptor must be a string or object";
        return false;
    }

    if (rel_path.empty()) {
        return true;
    }

    const std::filesystem::path absolute_path = base_dir / rel_path;
    out_desc.path = absolute_path;
    std::vector<std::uint8_t> bytes;
    if (!read_binary_file(absolute_path, bytes, out_error)) {
        return false;
    }

    const std::string ascii_head = lowercase_ascii(std::string(
        reinterpret_cast<const char*>(bytes.data()),
        reinterpret_cast<const char*>(bytes.data() + std::min<std::size_t>(bytes.size(), 64))));
    if (ascii_head.find("placeholder") != std::string::npos) {
        out_error = "Shader binary still contains placeholder text: " + absolute_path.string();
        return false;
    }

    if (!validate_binary_magic(absolute_path, backend, bytes, out_error)) {
        return false;
    }

    if (out_desc.file_size_bytes != 0 && out_desc.file_size_bytes != bytes.size()) {
        out_error = "Shader file size mismatch for: " + absolute_path.string();
        return false;
    }

    const std::uint64_t computed_hash = fnv1a64_bytes(bytes);
    if (out_desc.hash_fnv1a64 != 0 && out_desc.hash_fnv1a64 != computed_hash) {
        std::ostringstream oss;
        oss << "Shader hash mismatch for " << absolute_path.string()
            << " expected=" << out_desc.hash_fnv1a64
            << " actual=" << computed_hash;
        out_error = oss.str();
        return false;
    }

    out_desc.hash_fnv1a64 = computed_hash;
    out_desc.file_size_bytes = static_cast<std::uint64_t>(bytes.size());
    out_desc.payload = std::move(bytes);
    out_desc.available = true;
    out_error.clear();
    return true;
}

} // namespace

bool ShaderPackageLibrary::load_from_manifest(const std::filesystem::path& manifest_path, std::string& out_error) {
    variants_.clear();
    manifest_version_ = 0;

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

    manifest_version_ = root.value("manifest_version", 0);
    if (manifest_version_ <= 0) {
        out_error = "Shader manifest must define manifest_version > 0";
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

        ShaderVariantPath variant{};
        if (it.value().contains("dxil")) {
            if (!parse_binary_descriptor(it.value()["dxil"], base_dir, "dxil", variant.dxil, out_error)) {
                out_error = "Variant '" + it.key() + "' dxil error: " + out_error;
                return false;
            }
        }
        if (it.value().contains("spirv")) {
            if (!parse_binary_descriptor(it.value()["spirv"], base_dir, "spirv", variant.spirv, out_error)) {
                out_error = "Variant '" + it.key() + "' spirv error: " + out_error;
                return false;
            }
        }

        if (!variant.dxil.available && !variant.spirv.available) {
            out_error = "Shader variant must provide at least one valid backend binary: " + it.key();
            return false;
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

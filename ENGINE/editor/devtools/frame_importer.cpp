#include "frame_importer.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "utils/stb_image.h"
#include "utils/stb_image_write.h"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>
#include <tuple>

namespace devmode::frame_importer {
namespace {

std::string lower_copy(const std::string& value) {
    return vibble::strings::to_lower_copy(value);
}

std::string path_string(const std::filesystem::path& path) {
    return path.generic_string();
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::filesystem::path make_staging_directory(const std::filesystem::path& output_dir,
                                             std::error_code& ec) {
    const std::filesystem::path parent = output_dir.has_parent_path()
        ? output_dir.parent_path()
        : std::filesystem::current_path(ec);
    if (ec) {
        return {};
    }

    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return {};
    }

    const std::string stem = output_dir.filename().empty()
        ? std::string{"frames"}
        : output_dir.filename().string();
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    for (int attempt = 0; attempt < 64; ++attempt) {
        std::filesystem::path candidate =
            parent / (".frame_import_tmp_" + stem + "_" + std::to_string(now) + "_" + std::to_string(attempt));
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            std::filesystem::create_directories(candidate, ec);
            if (!ec) {
                return candidate;
            }
        }
        ec.clear();
    }

    ec = std::make_error_code(std::errc::file_exists);
    return {};
}

std::filesystem::path make_backup_directory(const std::filesystem::path& output_dir,
                                            std::error_code& ec) {
    const std::filesystem::path parent = output_dir.has_parent_path()
        ? output_dir.parent_path()
        : std::filesystem::current_path(ec);
    if (ec) {
        return {};
    }

    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return {};
    }

    const std::string stem = output_dir.filename().empty()
        ? std::string{"frames"}
        : output_dir.filename().string();
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::filesystem::path candidate =
            parent / (".frame_import_backup_" + stem + "_" + std::to_string(now) + "_" + std::to_string(attempt));
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        ec.clear();
    }
    ec = std::make_error_code(std::errc::file_exists);
    return {};
}

bool validate_png_readable(const std::filesystem::path& path, std::string& error_message) {
    SDL_Surface* loaded = IMG_Load(path.string().c_str());
    if (!loaded) {
        error_message = "Written frame is not readable as an image '" + path_string(path) +
                        "': " + SDL_GetError();
        return false;
    }
    const bool valid = loaded->w > 0 && loaded->h > 0;
    SDL_DestroySurface(loaded);
    if (!valid) {
        error_message = "Written frame has invalid dimensions '" + path_string(path) + "'";
    }
    return valid;
}

bool validate_frame_sequence(const std::filesystem::path& folder,
                             int frame_count,
                             std::string& error_message) {
    for (int i = 0; i < frame_count; ++i) {
        const std::filesystem::path path = folder / (std::to_string(i) + ".png");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec || !std::filesystem::is_regular_file(path, ec) || ec) {
            error_message = "Missing written frame '" + path_string(path) + "'";
            return false;
        }
        if (!validate_png_readable(path, error_message)) {
            return false;
        }
    }
    return true;
}

bool write_regular_image_as_png(const std::filesystem::path& source,
                                const std::filesystem::path& destination,
                                std::string& error_message) {
    SDL_Surface* loaded = IMG_Load(source.string().c_str());
    if (!loaded) {
        error_message = "Failed to decode image '" + path_string(source) + "': " + SDL_GetError();
        return false;
    }

    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!converted) {
        error_message = "Failed to convert image '" + path_string(source) + "': " + SDL_GetError();
        return false;
    }

    const bool wrote = IMG_SavePNG(converted, destination.string().c_str());
    SDL_DestroySurface(converted);
    if (!wrote) {
        error_message = "Failed to write PNG frame '" + path_string(destination) + "': " + SDL_GetError();
        return false;
    }
    if (!validate_png_readable(destination, error_message)) {
        return false;
    }
    return true;
}

int write_gif_frames_to_staging(const std::filesystem::path& gif_path,
                                const std::filesystem::path& staging_dir,
                                int start_index,
                                std::vector<std::string>& warnings,
                                std::string& error_message) {
    std::vector<unsigned char> bytes;
    try {
        std::ifstream in(gif_path, std::ios::binary);
        if (!in) {
            error_message = "Failed to open GIF '" + path_string(gif_path) + "'";
            return 0;
        }
        in.unsetf(std::ios::skipws);
        bytes.insert(bytes.begin(), std::istream_iterator<unsigned char>(in), std::istream_iterator<unsigned char>());
    } catch (const std::exception& ex) {
        error_message = "Failed reading GIF '" + path_string(gif_path) + "': " + ex.what();
        return 0;
    }

    if (bytes.empty()) {
        error_message = "GIF file is empty: '" + path_string(gif_path) + "'";
        return 0;
    }

    int x = 0;
    int y = 0;
    int z = 0;
    int comp = 0;
    int* delays = nullptr;
    stbi_uc* data = stbi_load_gif_from_memory(bytes.data(),
                                              static_cast<int>(bytes.size()),
                                              &delays,
                                              &x,
                                              &y,
                                              &z,
                                              &comp,
                                              STBI_rgb_alpha);
    if (!data || x <= 0 || y <= 0 || z <= 0) {
        if (data) {
            stbi_image_free(data);
        }
        if (delays) {
            stbi_image_free(delays);
        }
        error_message = "Failed to decode GIF frames from '" + path_string(gif_path) + "'";
        return 0;
    }

    const int channels = 4;
    const int stride = x * channels;
    int written = 0;
    for (int i = 0; i < z; ++i) {
        const std::filesystem::path dst = staging_dir / (std::to_string(start_index + written) + ".png");
        const stbi_uc* frame = data +
            static_cast<std::size_t>(i) * static_cast<std::size_t>(x) * static_cast<std::size_t>(y) * channels;
        const int ok = stbi_write_png(dst.string().c_str(), x, y, channels, frame, stride);
        std::string validation_error;
        if (ok && validate_png_readable(dst, validation_error)) {
            ++written;
        } else {
            warnings.push_back(ok
                ? validation_error
                : "Failed to write GIF frame " + std::to_string(i) + " from '" + path_string(gif_path) + "'");
        }
    }

    stbi_image_free(data);
    if (delays) {
        stbi_image_free(delays);
    }

    if (written <= 0) {
        error_message = "No GIF frames were written from '" + path_string(gif_path) + "'";
    }
    return written;
}

bool replace_destination_frames(const std::filesystem::path& staging_dir,
                                const std::filesystem::path& output_dir,
                                int frame_count,
                                std::string& error_message) {
    std::error_code ec;
    if (!validate_frame_sequence(staging_dir, frame_count, error_message)) {
        return false;
    }

    const std::filesystem::path parent = output_dir.has_parent_path()
        ? output_dir.parent_path()
        : std::filesystem::path{};
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error_message = "Failed to prepare output parent '" + path_string(parent) + "': " + ec.message();
            return false;
        }
    }

    std::filesystem::path backup_dir;
    const bool output_existed = std::filesystem::exists(output_dir, ec);
    if (ec) {
        error_message = "Failed to inspect output folder '" + path_string(output_dir) + "': " + ec.message();
        return false;
    }

    auto restore_backup = [&]() {
        std::error_code restore_ec;
        std::filesystem::remove_all(output_dir, restore_ec);
        restore_ec.clear();
        if (!backup_dir.empty() && std::filesystem::exists(backup_dir, restore_ec) && !restore_ec) {
            std::filesystem::rename(backup_dir, output_dir, restore_ec);
        }
    };

    if (output_existed) {
        backup_dir = make_backup_directory(output_dir, ec);
        if (ec || backup_dir.empty()) {
            error_message = "Failed to allocate backup folder for '" + path_string(output_dir) +
                            "': " + ec.message();
            return false;
        }
        std::filesystem::rename(output_dir, backup_dir, ec);
        if (ec) {
            error_message = "Failed to backup output folder '" + path_string(output_dir) + "': " + ec.message();
            return false;
        }
    }

    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        error_message = "Failed to create output folder '" + path_string(output_dir) + "': " + ec.message();
        restore_backup();
        return false;
    }

    for (int i = 0; i < frame_count; ++i) {
        const std::filesystem::path src = staging_dir / (std::to_string(i) + ".png");
        const std::filesystem::path dst = output_dir / (std::to_string(i) + ".png");
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error_message = "Failed to install frame '" + path_string(dst) + "': " + ec.message();
            restore_backup();
            return false;
        }
    }

    if (!validate_frame_sequence(output_dir, frame_count, error_message)) {
        restore_backup();
        return false;
    }

    if (!backup_dir.empty()) {
        std::filesystem::remove_all(backup_dir, ec);
    }
    return true;
}

} // namespace

bool has_extension_ci(const std::filesystem::path& path, std::string_view extension) {
    return lower_copy(path.extension().string()) == lower_copy(std::string(extension));
}

bool is_gif_file(const std::filesystem::path& path) {
    return has_extension_ci(path, ".gif");
}

bool is_supported_image_file(const std::filesystem::path& path) {
    return has_extension_ci(path, ".png") ||
           has_extension_ci(path, ".jpg") ||
           has_extension_ci(path, ".jpeg") ||
           has_extension_ci(path, ".bmp") ||
           has_extension_ci(path, ".webp") ||
           has_extension_ci(path, ".gif");
}

std::vector<std::filesystem::path> normalize_sequence(const std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> normalized = files;
    auto numeric_key = [](const std::filesystem::path& path) {
        std::string stem = path.stem().string();
        try {
            return std::make_tuple(0, std::stoi(stem), lower_copy(stem), lower_copy(path.filename().string()));
        } catch (...) {
            std::smatch match;
            static const std::regex number_regex{"(\\d+)", std::regex::icase};
            if (std::regex_search(stem, match, number_regex)) {
                try {
                    return std::make_tuple(0, std::stoi(match.str(1)), lower_copy(stem), lower_copy(path.filename().string()));
                } catch (...) {
                }
            }
        }
        return std::make_tuple(1, 0, lower_copy(stem), lower_copy(path.filename().string()));
    };

    std::sort(normalized.begin(), normalized.end(), [&](const auto& lhs, const auto& rhs) {
        return numeric_key(lhs) < numeric_key(rhs);
    });
    return normalized;
}

FrameImportResult import_frames_to_directory(const std::vector<std::filesystem::path>& files,
                                             const std::filesystem::path& output_dir) {
    FrameImportResult result;
    result.output_directory = output_dir;
    if (output_dir.empty()) {
        result.failed_stage = "resolve_output";
        result.error_message = "Output folder is unavailable.";
        return result;
    }
    if (files.empty()) {
        result.failed_stage = "validate_input";
        result.error_message = "No image files were provided.";
        return result;
    }

    std::error_code ec;
    const std::filesystem::path staging_dir = make_staging_directory(output_dir, ec);
    if (ec || staging_dir.empty()) {
        result.failed_stage = "stage";
        result.error_message = "Failed to prepare temporary frame import folder for '" +
                               path_string(output_dir) + "': " + ec.message();
        return result;
    }

    const auto cleanup = [&]() {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(staging_dir, cleanup_ec);
    };

    int next_index = 0;
    for (const auto& source : normalize_sequence(files)) {
        if (!std::filesystem::exists(source, ec) || ec || !std::filesystem::is_regular_file(source, ec) || ec) {
            result.warnings.push_back("Skipped missing frame source '" + path_string(source) + "'");
            ec.clear();
            continue;
        }
        if (!is_supported_image_file(source)) {
            result.warnings.push_back("Skipped unsupported frame source '" + path_string(source) + "'");
            continue;
        }

        if (is_gif_file(source)) {
            std::string error;
            const int gif_written = write_gif_frames_to_staging(source, staging_dir, next_index, result.warnings, error);
            if (gif_written <= 0 && !error.empty()) {
                result.warnings.push_back(error);
            }
            next_index += gif_written;
            continue;
        }

        const std::filesystem::path dst = staging_dir / (std::to_string(next_index) + ".png");
        std::string error;
        if (write_regular_image_as_png(source, dst, error)) {
            ++next_index;
        } else {
            result.warnings.push_back(error);
        }
    }

    if (next_index <= 0) {
        result.failed_stage = "decode";
        result.error_message = result.warnings.empty()
            ? "No frames were imported."
            : result.warnings.front();
        cleanup();
        return result;
    }

    std::string replace_error;
    if (!replace_destination_frames(staging_dir, output_dir, next_index, replace_error)) {
        result.failed_stage = "install";
        result.error_message = replace_error;
        cleanup();
        return result;
    }

    result.frames_written = next_index;
    cleanup();
    return result;
}

FrameImportResult import_gif_to_directory(const std::filesystem::path& gif_path,
                                          const std::filesystem::path& output_dir) {
    return import_frames_to_directory({gif_path}, output_dir);
}

void cleanup_stale_import_folders(const std::filesystem::path& asset_root) {
    if (asset_root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(asset_root, ec) || ec || !std::filesystem::is_directory(asset_root, ec) || ec) {
        return;
    }
    for (std::filesystem::directory_iterator it(asset_root, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (!it->is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (starts_with(name, ".frame_import_tmp") ||
            starts_with(name, ".frame_import_backup") ||
            name.find("_frame_import_tmp_") != std::string::npos ||
            name.find(".defaults_backup_") == 0) {
            std::error_code remove_ec;
            std::filesystem::remove_all(it->path(), remove_ec);
        }
    }
}

} // namespace devmode::frame_importer

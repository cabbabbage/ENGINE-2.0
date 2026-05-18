#include "frame_importer.hpp"

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

std::filesystem::path make_staging_directory(const std::filesystem::path& output_dir,
                                             std::error_code& ec) {
    const std::filesystem::path parent = output_dir.has_parent_path()
        ? output_dir.parent_path()
        : std::filesystem::current_path(ec);
    if (ec) {
        return {};
    }

    const std::string stem = output_dir.filename().empty()
        ? std::string{"frames"}
        : output_dir.filename().string();
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    for (int attempt = 0; attempt < 64; ++attempt) {
        std::filesystem::path candidate =
            parent / ("." + stem + "_frame_import_tmp_" + std::to_string(now) + "_" + std::to_string(attempt));
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

bool write_regular_image_as_png(const std::filesystem::path& source,
                                const std::filesystem::path& destination,
                                std::string& error_message) {
    if (has_extension_ci(source, ".png")) {
        std::error_code ec;
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error_message = "Failed to copy '" + path_string(source) + "' to '" +
                            path_string(destination) + "': " + ec.message();
            return false;
        }
        return true;
    }

    int width = 0;
    int height = 0;
    int comp = 0;
    stbi_uc* pixels = stbi_load(source.string().c_str(), &width, &height, &comp, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) {
            stbi_image_free(pixels);
        }
        error_message = "Failed to decode image '" + path_string(source) + "'";
        return false;
    }

    const int wrote = stbi_write_png(destination.string().c_str(),
                                    width,
                                    height,
                                    STBI_rgb_alpha,
                                    pixels,
                                    width * STBI_rgb_alpha);
    stbi_image_free(pixels);
    if (!wrote) {
        error_message = "Failed to write PNG frame '" + path_string(destination) + "'";
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
        if (ok) {
            ++written;
        } else {
            warnings.push_back("Failed to write GIF frame " + std::to_string(i) +
                               " from '" + path_string(gif_path) + "'");
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
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        error_message = "Failed to prepare output folder '" + path_string(output_dir) + "': " + ec.message();
        return false;
    }

    for (std::filesystem::directory_iterator it(output_dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (!has_extension_ci(it->path(), ".png")) {
            continue;
        }
        std::filesystem::remove(it->path(), ec);
        if (ec) {
            error_message = "Failed to remove old frame '" + path_string(it->path()) + "': " + ec.message();
            return false;
        }
    }
    if (ec) {
        error_message = "Failed to list output folder '" + path_string(output_dir) + "': " + ec.message();
        return false;
    }

    for (int i = 0; i < frame_count; ++i) {
        const std::filesystem::path src = staging_dir / (std::to_string(i) + ".png");
        const std::filesystem::path dst = output_dir / (std::to_string(i) + ".png");
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error_message = "Failed to install frame '" + path_string(dst) + "': " + ec.message();
            return false;
        }
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
    if (output_dir.empty()) {
        result.error_message = "Output folder is unavailable.";
        return result;
    }
    if (files.empty()) {
        result.error_message = "No image files were provided.";
        return result;
    }

    std::error_code ec;
    const std::filesystem::path staging_dir = make_staging_directory(output_dir, ec);
    if (ec || staging_dir.empty()) {
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
        result.error_message = result.warnings.empty()
            ? "No frames were imported."
            : result.warnings.front();
        cleanup();
        return result;
    }

    std::string replace_error;
    if (!replace_destination_frames(staging_dir, output_dir, next_index, replace_error)) {
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

} // namespace devmode::frame_importer

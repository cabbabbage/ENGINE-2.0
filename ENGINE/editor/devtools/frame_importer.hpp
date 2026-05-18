#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace devmode::frame_importer {

struct FrameImportResult {
    int frames_written = 0;
    std::string error_message;
    std::vector<std::string> warnings;

    bool success() const { return frames_written > 0 && error_message.empty(); }
};

bool has_extension_ci(const std::filesystem::path& path, std::string_view extension);
bool is_supported_image_file(const std::filesystem::path& path);
bool is_gif_file(const std::filesystem::path& path);

std::vector<std::filesystem::path> normalize_sequence(const std::vector<std::filesystem::path>& files);

FrameImportResult import_frames_to_directory(const std::vector<std::filesystem::path>& files,
                                             const std::filesystem::path& output_dir);

FrameImportResult import_gif_to_directory(const std::filesystem::path& gif_path,
                                          const std::filesystem::path& output_dir);

} // namespace devmode::frame_importer

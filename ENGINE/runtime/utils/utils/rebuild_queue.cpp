#include "utils/rebuild_queue.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <sstream>
#include <vector>
#include <cerrno>

#ifdef _WIN32
#include <process.h>
#endif

#include "core/manifest/manifest_loader.hpp"
#include "utils/log.hpp"

namespace fs = std::filesystem;

namespace vibble {
using json = nlohmann::json;
namespace {

fs::path default_repo_root() {
    fs::path manifest = manifest::manifest_path();
    if (manifest.empty()) {
        return fs::current_path();
    }
    return fs::absolute(manifest).parent_path();
}

static inline std::string exe_ext() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

fs::path tool_path(const fs::path& repo_root, const std::string& tool_name) {
    const fs::path tool_exe = tool_name + exe_ext();
    const fs::path root_tool = repo_root / "tools" / tool_exe;
    if (fs::exists(root_tool)) {
        return root_tool;
    }
    return repo_root / "ENGINE" / "tools" / tool_exe;
}

}

RebuildQueueCoordinator::RebuildQueueCoordinator() {
    repo_root_ = default_repo_root();
    manifest_path_ = fs::absolute(manifest::manifest_path());
    cache_root_ = repo_root_ / "cache";

    // Ensure the cache root exists before any tool runs; missing directories
    // previously caused downstream tools to bail out without generating images.
    std::error_code ec;
    fs::create_directories(cache_root_, ec);
    if (ec) {
        vibble::log::warn(std::string{"[RebuildQueue] Failed to create cache root '"} + cache_root_.string() + "': " + ec.message());
    }
}

void RebuildQueueCoordinator::request_full_asset_rebuild() const {
    mark_all_frames_for_rebuild();
}

void RebuildQueueCoordinator::request_asset(const std::string& asset_name,
                                            const std::vector<std::string>& animations) const {
    if (asset_name.empty()) {
        return;
    }
    if (animations.empty()) {
        mark_asset_for_rebuild(asset_name);
        return;
    }
    for (const auto& anim : animations) {
        request_animation(asset_name, anim);
    }
}

void RebuildQueueCoordinator::request_animation(const std::string& asset_name,
                                                const std::string& animation) const {
    if (asset_name.empty() || animation.empty()) {
        return;
    }
    mark_animation_for_rebuild(asset_name, animation);
}

void RebuildQueueCoordinator::request_frame(const std::string& asset_name,
                                            const std::string& animation,
                                            int frame_index) const {
    if (asset_name.empty() || animation.empty() || frame_index < 0) {
        return;
    }
    mark_frame_for_rebuild(asset_name, animation, frame_index);
}

bool RebuildQueueCoordinator::has_pending_asset_work() const {
    return manifest_has_needs_rebuild();
}

bool RebuildQueueCoordinator::run_asset_tool(const std::string& command_prefix) const {
    const fs::path tool = tool_path(repo_root_, "asset_tool_cli");
    return run_cpp_tool(tool,
                        {"--manifest", manifest_path_.string(), "--cache-root", cache_root_.string()},
                        command_prefix);
}

void RebuildQueueCoordinator::mark_all_frames_for_rebuild() const {
    const fs::path tool = tool_path(repo_root_, "set_rebuild_cli");
    run_cpp_tool(tool, {"all", "--manifest", manifest_path_.string()}, "");
}

void RebuildQueueCoordinator::mark_asset_for_rebuild(const std::string& asset_name) const {
    const fs::path tool = tool_path(repo_root_, "set_rebuild_cli");
    run_cpp_tool(tool, {"asset", asset_name, "--manifest", manifest_path_.string()}, "");
}

void RebuildQueueCoordinator::mark_animation_for_rebuild(const std::string& asset_name,
                                                         const std::string& animation) const {
    const fs::path tool = tool_path(repo_root_, "set_rebuild_cli");
    run_cpp_tool(tool,
                 {"animation", asset_name, animation, "--manifest", manifest_path_.string()},
                 "");
}

void RebuildQueueCoordinator::mark_frame_for_rebuild(const std::string& asset_name,
                                                     const std::string& animation,
                                                     int frame_index) const {
    const fs::path tool = tool_path(repo_root_, "set_rebuild_cli");
    run_cpp_tool(tool,
                 {"frame", asset_name, animation, std::to_string(frame_index), "--manifest", manifest_path_.string()},
                 "");
}


bool RebuildQueueCoordinator::manifest_has_needs_rebuild() const {
    std::ifstream in(manifest_path_);
    if (!in.good()) {
        return false;
    }
    json manifest_json;
    try {
        in >> manifest_json;
    } catch (...) {
        return false;
    }
    auto assets_it = manifest_json.find("assets");
    if (assets_it == manifest_json.end() || !assets_it->is_object()) {
        return false;
    }
    for (auto it = assets_it->begin(); it != assets_it->end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        auto anims_it = it->find("animations");
        if (anims_it == it->end() || !anims_it->is_object()) {
            continue;
        }
        json anims = *anims_it;
        if (anims.contains("animations") && anims["animations"].is_object()) {
            anims = anims["animations"];
        }
        for (auto a_it = anims.begin(); a_it != anims.end(); ++a_it) {
            if (!a_it->is_object()) {
                continue;
            }
            const json& anim = *a_it;
            auto frames_it = anim.find("frames");
            if (frames_it == anim.end() || !frames_it->is_array()) {
                continue;
            }
            for (const auto& frame_entry : *frames_it) {
                if (frame_entry.is_object()) {
                    auto flag = frame_entry.find("needs_rebuild");
                    if (flag != frame_entry.end() && flag->is_boolean() && flag->get<bool>()) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}


bool RebuildQueueCoordinator::run_cpp_tool(const fs::path& tool,
                                           const std::vector<std::string>& args,
                                           const std::string& command_prefix) const {
    if (!fs::exists(tool)) {
        vibble::log::error(std::string{"Missing C++ tool: "} + tool.string());
        return false;
    }

    auto render_command = [](const std::vector<std::string>& argv) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& token : argv) {
            if (!first) {
                oss << ' ';
            }
            first = false;
            oss << '"' << token << '"';
        }
        return oss.str();
    };

    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(tool.string());
    argv.insert(argv.end(), args.begin(), args.end());

    const std::string command_text = render_command(argv);
    vibble::log::info(std::string{"[RebuildQueue] Running "} + tool.filename().string());

    // If a prefix is explicitly provided, preserve legacy shell behaviour.
    if (!command_prefix.empty()) {
        const std::string full_command = command_prefix + command_text;
        int ret = std::system(full_command.c_str());
        if (ret != 0) {
            vibble::log::warn(std::string{"[RebuildQueue] Tool exited with code "} + std::to_string(ret));
            vibble::log::debug(std::string{"[RebuildQueue] Command: "} + full_command);
            return false;
        }
        return true;
    }

#ifdef _WIN32
    // Spawn directly to avoid cmd.exe quoting issues seen in the logs.
    std::vector<std::wstring> wargs;
    wargs.reserve(argv.size());
    for (const auto& arg : argv) {
        wargs.emplace_back(std::filesystem::path(arg).native());
    }

    std::vector<wchar_t*> wargv;
    wargv.reserve(wargs.size() + 1);
    for (auto& w : wargs) {
        wargv.push_back(w.data());
    }
    wargv.push_back(nullptr);

    intptr_t ret = _wspawnv(_P_WAIT, tool.c_str(), wargv.data());
    if (ret == -1) {
        vibble::log::warn(std::string{"[RebuildQueue] Failed to launch tool: "} + command_text + " (" + std::to_string(errno) + ")");
        return false;
    }
    if (ret != 0) {
        vibble::log::warn(std::string{"[RebuildQueue] Tool exited with code "} + std::to_string(ret));
        vibble::log::debug(std::string{"[RebuildQueue] Command: "} + command_text);
        return false;
    }
    return true;
#else
    int ret = std::system(command_text.c_str());
    if (ret != 0) {
        vibble::log::warn(std::string{"[RebuildQueue] Tool exited with code "} + std::to_string(ret));
        vibble::log::debug(std::string{"[RebuildQueue] Command: "} + command_text);
        return false;
    }
    return true;
#endif
}

bool RebuildQueueCoordinator::validate_manifest_cache(const std::string& command_prefix) const {
    std::error_code ec;
    if (!fs::is_directory(cache_root_)) {
        fs::create_directories(cache_root_, ec);
        if (ec) {
            vibble::log::warn(std::string{"[RebuildQueue] Cache root missing and could not be created: "} + ec.message());
        }
    }

    if (!fs::is_directory(cache_root_)) {
        vibble::log::warn("[RebuildQueue] Cache root missing; queueing full asset rebuild.");
        mark_all_frames_for_rebuild();
    }
    const fs::path tool = tool_path(repo_root_, "cache_validator_cli");
    if (!run_cpp_tool(tool, {"--manifest", manifest_path_.string()}, command_prefix)) {
        vibble::log::warn("[RebuildQueue] Cache validation failed; queueing full asset rebuild.");
        mark_all_frames_for_rebuild();
        return false;
    }
    return true;
}

}

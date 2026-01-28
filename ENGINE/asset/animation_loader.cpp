#include "animation_loader.hpp"
#include "animation.hpp"
#include "animation_cloner.hpp"
#include "asset/asset_info.hpp"
#include "asset/asset_types.hpp"
#include "asset/surface_utils.hpp"
#include "utils/cache_manager.hpp"
#include "render/render.hpp"
#include "utils/loading_status_notifier.hpp"
#include "utils/log.hpp"
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace {

void resolve_inherited_movements(AssetInfo& info) {
        for (auto& [name, anim] : info.animations) {
                if (!anim.inherit_source_movement || anim.source.kind != "animation" || anim.source.name.empty()) {
                        continue;
                }
                auto it = info.animations.find(anim.source.name);
                if (it == info.animations.end()) continue;

                bool has_frames = false;
                for (std::size_t path_idx = 0; path_idx < anim.movement_path_count(); ++path_idx) {
                        const auto& path = anim.movement_path(path_idx);
                        if (!path.empty()) {
                                has_frames = true;
                                break;
                        }
                }
                if (has_frames) continue;
                anim.inherit_movement_from(it->second);
        }
}

bool json_bool(const nlohmann::json& value, bool fallback) {
        if (value.is_boolean()) {
                return value.get<bool>();
        }
        if (value.is_number_integer()) {
                return value.get<int>() != 0;
        }
        if (value.is_number()) {
                return value.get<double>() != 0.0;
        }
        if (value.is_string()) {
                std::string text = value.get<std::string>();
                std::string lowered;
                lowered.reserve(text.size());
                for (unsigned char ch : text) {
                        lowered.push_back(static_cast<char>(std::tolower(ch)));
                }
                if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
                        return true;
                }
                if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
                        return false;
                }
        }
        return fallback;
}

int json_int(const nlohmann::json& value, int fallback) {
        if (value.is_number_integer()) {
                return value.get<int>();
        }
        if (value.is_number()) {
                return static_cast<int>(value.get<double>());
        }
        if (value.is_string()) {
                try {
                        return std::stoi(value.get<std::string>());
                } catch (...) {
                }
        }
        return fallback;
}

float json_float(const nlohmann::json& value, float fallback) {
        if (value.is_number()) {
                return static_cast<float>(value.get<double>());
        }
        if (value.is_string()) {
                try {
                        return std::stof(value.get<std::string>());
                } catch (...) {
                }
        }
        return fallback;
}

std::vector<float> discover_cached_scale_steps(const fs::path& cache_folder) {
        std::vector<float> steps;
        std::error_code ec;
        if (!fs::exists(cache_folder, ec) || !fs::is_directory(cache_folder, ec)) {
                return steps;
        }

        for (const auto& entry : fs::directory_iterator(cache_folder, ec)) {
                if (ec) break;
                if (!entry.is_directory()) continue;
                const std::string name = entry.path().filename().string();
                const std::string prefix = "scale_";
                if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size()) {
                        continue;
                }
                try {
                        const int pct = std::stoi(name.substr(prefix.size()));
                        if (pct <= 0) continue;
                        float step = static_cast<float>(pct) * 0.01f;
                        if (!std::isfinite(step) || step <= 0.0f || step >= 0.999f) {
                                continue;
                        }
                        steps.push_back(std::clamp(step, 0.01f, 0.99f));
                } catch (...) {
                        continue;
                }
        }

        if (steps.empty()) {
                return steps;
        }

        std::sort(steps.begin(), steps.end(), std::greater<float>());
        steps.erase(std::unique(steps.begin(), steps.end(), [](float a, float b) {
                return std::fabs(a - b) < 1e-4f;
        }), steps.end());

        if (steps.size() > render_pipeline::ScalingLogic::kMaxVariantCount) {
                steps.resize(render_pipeline::ScalingLogic::kMaxVariantCount);
        }

        return steps;
}

AnimationChildFrameData make_default_child_frame(int child_index) {
        AnimationChildFrameData sample{};
        sample.child_index = child_index;
        sample.dx = 0;
        sample.dy = 0;
        sample.dz = 0;
        sample.degree = 0.0f;
        sample.visible = false;
        return sample;
}

std::optional<AnimationChildFrameData> parse_child_frame_sample(const nlohmann::json& node,
                                                                int child_index,
                                                                const std::string& asset_name) {
        if (!node.is_object()) {
                std::cout << "[AnimationLoader] child timeline frame for asset '" << asset_name
                          << "' is not an object; legacy child formats are unsupported.\n";
                return std::nullopt;
        }
        if (node.contains("render_in_front")) {
                std::cout << "[AnimationLoader] child timeline frame for asset '" << asset_name
                          << "' includes legacy field 'render_in_front' and cannot be loaded. "
                          << "child_timelines are the only supported child source.\n";
                return std::nullopt;
        }
        AnimationChildFrameData sample = make_default_child_frame(child_index);
        sample.dx = json_int(node.value("dx", 0), 0);
        sample.dy = json_int(node.value("dy", 0), 0);
        // dz is stored as float percentage (0.0-1.0) of parent height
        sample.dz = json_float(node.value("dz", 0.0f), 0.0f);
        if (!node.contains("dz")) {
                // Legacy fallback: if dz not present, use dy as dz
                sample.dz = static_cast<float>(sample.dy);
                sample.dy = 0;
        }
        if (node.contains("degree")) {
                sample.degree = json_float(node["degree"], 0.0f);
        } else if (node.contains("rotation")) {
                sample.degree = json_float(node["rotation"], 0.0f);
        }
        sample.visible = json_bool(node.value("visible", sample.visible), sample.visible);
        return sample;
}

std::optional<AnimationChildMode> parse_child_mode(const nlohmann::json& node) {
        if (!node.contains("mode") || !node["mode"].is_string()) {
                return std::nullopt;
        }
        std::string mode = node["mode"].get<std::string>();
        std::string lowered;
        lowered.reserve(mode.size());
        for (unsigned char ch : mode) {
                lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
        if (lowered == "static") {
                return AnimationChildMode::Static;
        }
        if (lowered == "async" || lowered == "asynchronous") {
                return AnimationChildMode::Async;
        }
        return std::nullopt;
}

int count_png_files(const std::string& folder) {
        int count = 0;
        const fs::path folder_path(folder);

        std::error_code ec;
        if (!fs::exists(folder_path, ec) || ec) {
                return 0;
        }

        while (true) {
                fs::path frame_path = folder_path / (std::to_string(count) + ".png");
                if (!fs::exists(frame_path, ec) || ec) {
                        break;
                }
                ++count;
        }

        return count;
}

fs::path project_root_path() {
#ifdef PROJECT_ROOT
        return fs::path(PROJECT_ROOT);
#else
        return fs::current_path();
#endif
}

float read_float(const nlohmann::json& value, float fallback = 0.0f) {
        if (value.is_number()) {
                try {
                        return static_cast<float>(value.get<double>());
                } catch (...) {}
        }
        if (value.is_string()) {
                try {
                        return std::stof(value.get<std::string>());
                } catch (...) {}
        }
        return fallback;
}

int read_int(const nlohmann::json& value, int fallback = 0) {
        if (value.is_number_integer()) {
                try {
                        return value.get<int>();
                } catch (...) {}
        } else if (value.is_number()) {
                try {
                        return static_cast<int>(value.get<double>());
                } catch (...) {}
        } else if (value.is_string()) {
                try {
                        return std::stoi(value.get<std::string>());
                } catch (...) {}
        }
        return fallback;
}

void append_hit_box(animation_update::FrameHitGeometry& geometry,
                    const nlohmann::json& node) {
        if (node.is_null()) {
                return;
        }
        animation_update::FrameHitGeometry::HitBox box;
        if (!node.is_object()) {
                return;
        }
        box.center_x = read_float(node.value("center_x", 0.0f));
        const bool has_center_z = node.contains("center_z");
        box.center_y = has_center_z ? read_float(node.value("center_y", 0.0f)) : 0.0f;
        box.center_z = has_center_z ? read_float(node.value("center_z", 0.0f))
                                    : read_float(node.value("center_y", 0.0f));
        box.half_width = read_float(node.value("half_width", 0.0f));
        box.half_height = read_float(node.value("half_height", 0.0f));
        box.rotation_degrees = read_float(node.value("rotation", node.value("rotation_degrees", 0.0f)));
        if (box.is_empty()) {
                return;
        }
        geometry.boxes.push_back(box);
}

void apply_hit_geometry_entry(AnimationFrame& frame, const nlohmann::json& entry) {
        frame.hit_geometry.boxes.clear();
        if (entry.is_object()) {
                if (entry.contains("boxes") && entry["boxes"].is_array()) {
                        for (const auto& node : entry["boxes"]) {
                                append_hit_box(frame.hit_geometry, node);
                        }
                        return;
                }
                if (entry.contains("center_x") || entry.contains("half_width") || entry.contains("rotation")) {
                        append_hit_box(frame.hit_geometry, entry);
                        return;
                }
        }
}

void append_attack_vector(animation_update::FrameAttackGeometry& geometry,
                          const nlohmann::json& node) {
        if (node.is_null()) {
                return;
        }
        animation_update::FrameAttackGeometry::Vector vec;
        if (!node.is_object()) {
                return;
        }
        vec.start_x = read_float(node.value("start_x", 0.0f));
        const bool has_start_z = node.contains("start_z");
        vec.start_y = has_start_z ? read_float(node.value("start_y", 0.0f)) : 0.0f;
        vec.start_z = has_start_z ? read_float(node.value("start_z", 0.0f))
                                  : read_float(node.value("start_y", 0.0f));
        const bool has_control = node.contains("control_x") || node.contains("control_y") || node.contains("control_z");
        const bool has_control_z = node.contains("control_z");
        if (has_control) {
                vec.control_x = read_float(node.value("control_x", vec.start_x));
                vec.control_y = has_control_z ? read_float(node.value("control_y", vec.start_y)) : 0.0f;
                vec.control_z = has_control_z ? read_float(node.value("control_z", vec.start_z))
                                              : read_float(node.value("control_y", vec.start_z));
        } else {
                vec.control_x = (vec.start_x + read_float(node.value("end_x", 0.0f))) * 0.5f;
                const float fallback_end_z = read_float(node.value("end_z", read_float(node.value("end_y", 0.0f))));
                const float fallback_end_y = has_start_z ? read_float(node.value("end_y", 0.0f)) : 0.0f;
                vec.control_y = (vec.start_y + fallback_end_y) * 0.5f;
                vec.control_z = (vec.start_z + fallback_end_z) * 0.5f;
        }
        vec.end_x   = read_float(node.value("end_x", 0.0f));
        const bool has_end_z = node.contains("end_z");
        vec.end_y   = has_end_z ? read_float(node.value("end_y", 0.0f)) : 0.0f;
        vec.end_z   = has_end_z ? read_float(node.value("end_z", 0.0f))
                                : read_float(node.value("end_y", 0.0f));
        vec.damage  = read_int(node.value("damage", 0));
        geometry.add_vector(vec);
}

void apply_attack_geometry_entry(AnimationFrame& frame, const nlohmann::json& entry) {
        frame.attack_geometry.vectors.clear();
        if (entry.is_object()) {
                if (entry.contains("vectors") && entry["vectors"].is_array()) {
                        for (const auto& vec_node : entry["vectors"]) {
                                append_attack_vector(frame.attack_geometry, vec_node);
                        }
                        return;
                }
        }
}

void apply_combat_geometry(std::vector<std::vector<AnimationFrame>>& paths,
                           const nlohmann::json& hit_geometry,
                           const nlohmann::json& attack_geometry) {
        const nlohmann::json empty_json = nlohmann::json();
        const bool has_hit = hit_geometry.is_array();
        const bool has_attack = attack_geometry.is_array();
        for (auto& path : paths) {
                for (std::size_t idx = 0; idx < path.size(); ++idx) {
                        AnimationFrame& frame = path[idx];
                        const nlohmann::json& hit_entry = (has_hit && idx < hit_geometry.size()) ? hit_geometry[idx] : empty_json;
                        const nlohmann::json& attack_entry = (has_attack && idx < attack_geometry.size()) ? attack_geometry[idx] : empty_json;
                        apply_hit_geometry_entry(frame, hit_entry);
                        apply_attack_geometry_entry(frame, attack_entry);
                }
        }
}

bool path_exists_safely(const fs::path& path) {
        std::error_code ec;
        return fs::exists(path, ec);
}

std::string format_steps(const std::vector<float>& steps) {
        std::ostringstream oss;
        oss << '[';
        for (std::size_t i = 0; i < steps.size(); ++i) {
                if (i != 0) {
                        oss << ", ";
                }
                oss << std::fixed << std::setprecision(2) << steps[i];
        }
        oss << ']';
        return oss.str();
}

struct VariantLayerPaths {
        std::string scale_folder;
        std::string normal_folder;
        std::string foreground_folder;
        std::string background_folder;
};

VariantLayerPaths build_variant_layer_paths(const std::string& cache_folder,
                                            const std::vector<float>& steps,
                                            std::size_t index) {
        VariantLayerPaths paths;
        paths.scale_folder     = render_pipeline::ScalingLogic::VariantFolder(cache_folder, steps, index);
        const fs::path scale_root(paths.scale_folder);
        paths.normal_folder     = (scale_root / "normal").string();
        paths.foreground_folder = (scale_root / "foreground").string();
        paths.background_folder = (scale_root / "background").string();

        return paths;
}

inline double sanitize_scale_factor(float value) {
        if (!std::isfinite(value) || value < 0.0f) {
                return 1.0;
        }
        return static_cast<double>(value);
}

inline int scaled_dimension(int base, double scale) {
        if (base <= 0) {
                return 0;
        }
        if (scale <= 0.0) {
                return 0;
        }
        const long long rounded = std::llround(static_cast<double>(base) * scale);
        if (rounded < 1) {
                return 1;
        }
        if (rounded > static_cast<long long>(std::numeric_limits<int>::max())) {
                return std::numeric_limits<int>::max();
        }
        return static_cast<int>(rounded);
}

using AudioCache = std::unordered_map<std::string, std::weak_ptr<Mix_Chunk>>;

AudioCache& get_audio_cache() {
        static AudioCache cache;
        return cache;
}

std::shared_ptr<Mix_Chunk> load_audio_clip(const std::string& path) {
        if (path.empty()) return {};
        auto& cache = get_audio_cache();
        auto it = cache.find(path);
        if (it != cache.end()) {
                if (auto existing = it->second.lock()) {
                        return existing;
                }
        }
        if (!std::filesystem::exists(path)) {
                std::cerr << "[Animation] Audio file not found: " << path << "\n";
                return {};
        }
        Mix_Chunk* raw = Mix_LoadWAV(path.c_str());
        if (!raw) {
                std::cerr << "[Animation] Failed to load audio '" << path << "': " << Mix_GetError() << "\n";
                return {};
        }
        std::shared_ptr<Mix_Chunk> chunk(raw, Mix_FreeChunk);
        cache[path] = chunk;
        return chunk;
}

#if SDL_VERSION_ATLEAST(2,0,12)
void apply_scale_mode(SDL_Texture* tex, const AssetInfo& info) {
        if (tex) {
                SDL_SetTextureScaleMode(tex, info.smooth_scaling ? SDL_ScaleModeBest : SDL_ScaleModeNearest);
        }
}
#else
void apply_scale_mode(SDL_Texture*, const AssetInfo&) {}
#endif

}

void AnimationLoader::load(Animation& animation,
                     const std::string& trigger,
                     const nlohmann::json& anim_json,
                     AssetInfo& info,
                     const std::string& dir_path,
                     const std::string& root_cache,
                     float scale_factor,
                     SDL_Renderer* renderer,
                     SDL_Texture*& base_sprite,
                     int& scaled_sprite_w,
                     int& scaled_sprite_h,
                     int& original_canvas_width,
                     int& original_canvas_height,
                     bool scaling_refresh_pending,
                     LoadDiagnostics* diagnostics)
{
        const auto load_start = std::chrono::steady_clock::now();
        bool       loaded_from_cache = false;
        bool       reused_animation  = false;
        bool       cache_invalid_detected = false;
        const auto flush_diagnostics = [&]() {
                if (diagnostics) {
                        diagnostics->cache_invalid = diagnostics->cache_invalid || cache_invalid_detected;
                }
};
        const double safe_scale = sanitize_scale_factor(scale_factor);
        animation.clear_texture_cache();
        const bool prefer_cached = !scaling_refresh_pending;

        const bool supports_depthcue_cache = false;
        bool effect_hash_mismatch = false;
        animation.variant_steps_ = info.scale_variants;

        const fs::path cache_folder_path = fs::path(root_cache) / trigger;
        if (animation.variant_steps_.empty()) {
                animation.variant_steps_ = discover_cached_scale_steps(cache_folder_path);
        }
        if (animation.variant_steps_.empty()) {
                render_pipeline::ScalingLogic::NormalizeVariantSteps(animation.variant_steps_);
        }

        if (anim_json.contains("source")) {
                const auto& s = anim_json["source"];
                try {
                        if (s.contains("kind") && s["kind"].is_string())
                        animation.source.kind = s["kind"].get<std::string>();
			else
			animation.source.kind = "folder";
		} catch (...) { animation.source.kind = "folder"; }
		try {
			if (s.contains("path") && s["path"].is_string())
			animation.source.path = s["path"].get<std::string>();
			else
			animation.source.path.clear();
		} catch (...) { animation.source.path.clear(); }
		try {
			if (s.contains("name") && s["name"].is_string())
			animation.source.name = s["name"].get<std::string>();
			else
			animation.source.name.clear();
		} catch (...) { animation.source.name.clear(); }
	}

        if (animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        const Animation& src_anim = it->second;
                        if (!src_anim.variant_steps_.empty()) {
                                animation.variant_steps_ = src_anim.variant_steps_;
                        }
                }
        }

        const std::size_t initial_variant_count = animation.variant_steps_.size();

        if (info.scale_variants.empty() && !animation.variant_steps_.empty()) {
                info.scale_variants = animation.variant_steps_;
        }

        animation.flipped_source = anim_json.value("flipped_source", false);
        animation.flip_vertical_source = anim_json.value("flip_vertical_source", false);
        animation.flip_movement_horizontal = anim_json.value("flip_movement_horizontal", false);
        animation.flip_movement_vertical = anim_json.value("flip_movement_vertical", false);
        animation.reverse_source = anim_json.value("reverse_source", false);
        const bool inherit_source_movement = anim_json.value("inherit_source_movement", (animation.source.kind == "animation"));
        if (animation.source.kind == "animation" && anim_json.contains("derived_modifiers") &&
            anim_json["derived_modifiers"].is_object()) {
                const auto& modifiers = anim_json["derived_modifiers"];
                animation.reverse_source = modifiers.value("reverse", animation.reverse_source);
                animation.flipped_source = modifiers.value("flipX", animation.flipped_source);
                animation.flip_vertical_source = modifiers.value("flipY", animation.flip_vertical_source);
                animation.flip_movement_horizontal = modifiers.value("flipMovementX", animation.flip_movement_horizontal);
                animation.flip_movement_vertical = modifiers.value("flipMovementY", animation.flip_movement_vertical);
        } else if (animation.source.kind != "animation") {
                animation.flip_vertical_source = false;
                animation.flip_movement_horizontal = false;
                animation.flip_movement_vertical = false;
        }
        animation.inherit_source_movement = (animation.source.kind == "animation") && inherit_source_movement;

        animation.locked         = anim_json.value("locked", false);
	animation.loop      = anim_json.value("loop", true);
	animation.randomize = anim_json.value("randomize", false);
	animation.rnd_start = anim_json.value("rnd_start", false);
        animation.on_end_animation = anim_json.value("on_end", std::string{"default"});
        animation.on_end_behavior = Animation::classify_on_end(animation.on_end_animation);
        animation.child_asset_names_.clear();
        if (!info.animation_children.empty()) {
                animation.child_asset_names_ = info.animation_children;
        } else if (anim_json.contains("children") && anim_json["children"].is_array()) {
                for (const auto& child_entry : anim_json["children"]) {
                        if (!child_entry.is_string()) continue;
                        std::string name = child_entry.get<std::string>();
                        if (!name.empty()) {
                                animation.child_asset_names_.push_back(std::move(name));
                        }
                }
        }
        if (animation.child_asset_names_.empty() && animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto src_child_it = info.animations.find(animation.source.name);
                if (src_child_it != info.animations.end()) {
                        animation.child_asset_names_ = src_child_it->second.child_assets();
                }
        }

        if (!animation.child_asset_names_.empty()) {
                std::unordered_set<std::string> seen;
                std::vector<std::string> unique;
                unique.reserve(animation.child_asset_names_.size());
                for (const auto& n : animation.child_asset_names_) {
                        if (n.empty()) continue;
                        if (seen.insert(n).second) {
                                unique.push_back(n);
                        }
                }
                animation.child_asset_names_.swap(unique);
        }
        animation.total_dx = 0;
        animation.total_dy = 0;
        animation.total_dz = 0;
        animation.movement_paths_.clear();
        animation.audio_clip = Animation::AudioClip{};
        bool movement_specified = false;
        bool legacy_movement_children_found = false;
        nlohmann::json hit_geometry_json = nlohmann::json::array();
        if (anim_json.contains("hit_geometry") && anim_json["hit_geometry"].is_array()) {
                hit_geometry_json = anim_json["hit_geometry"];
        }
        nlohmann::json attack_geometry_json = nlohmann::json::array();
        if (anim_json.contains("attack_geometry") && anim_json["attack_geometry"].is_array()) {
                attack_geometry_json = anim_json["attack_geometry"];
        }

        auto parse_movement_sequence = [&](const nlohmann::json& seq, std::vector<AnimationFrame>& dest) {
                bool specified = false;
                if (!seq.is_array()) return specified;
                auto clamp = [](int v) { return (v < 0) ? 0 : (v > 255 ? 255 : v); };
                for (const auto& mv : seq) {
                        AnimationFrame fm;

                        if (mv.is_object()) {
                                try { fm.dx = static_cast<int>(mv.value("dx", 0)); } catch (...) { fm.dx = 0; }
                                try { fm.dy = static_cast<int>(mv.value("dy", 0)); } catch (...) { fm.dy = 0; }
                                try { fm.dz = static_cast<int>(mv.value("dz", 0)); } catch (...) { fm.dz = 0; }
                                fm.z_resort = mv.value("resort_z", false);
                                if (mv.contains("children")) {
                                        legacy_movement_children_found = true;
                                }
                                if (fm.dx != 0 || fm.dy != 0 || fm.dz != 0 || mv.contains("resort_z")) specified = true;
                                dest.push_back(std::move(fm));
                                continue;
                        }

                        if (!mv.is_array() || mv.size() < 2) continue;
                        try { fm.dx = mv[0].get<int>(); } catch (...) { fm.dx = 0; }
                        try { fm.dy = mv[1].get<int>(); } catch (...) { fm.dy = 0; }
                        fm.dz = 0;
                        if (mv.size() >= 3 && mv[2].is_number()) {
                                try { fm.dz = mv[2].get<int>(); } catch (...) { fm.dz = 0; }
                        } else if (mv.size() >= 3 && mv[2].is_boolean()) {
                                fm.z_resort = mv[2].get<bool>();
                        }
                        if (mv.size() >= 4 && mv[3].is_boolean()) {
                                fm.z_resort = mv[3].get<bool>();
                        }

                        for (const auto& node : mv) {
                                if (!node.is_array()) continue;
                                const bool looks_color = (node.size() == 3) && node[0].is_number() && node[1].is_number() && node[2].is_number();
                                if (looks_color) {
                                        int r = 255, g = 255, b = 255;
                                        try { r = clamp(node[0].get<int>()); } catch (...) { r = 255; }
                                        try { g = clamp(node[1].get<int>()); } catch (...) { g = 255; }
                                        try { b = clamp(node[2].get<int>()); } catch (...) { b = 255; }
                                        fm.rgb = SDL_Color{ static_cast<Uint8>(r), static_cast<Uint8>(g), static_cast<Uint8>(b), 255 };
                                        break;
                                }
                        }

                        for (const auto& node : mv) {
                                if (node.is_array() && !node.empty() && node[0].is_array()) {
                                        legacy_movement_children_found = true;
                                        break;
                                }
                        }
                        if (fm.dx != 0 || fm.dy != 0 || fm.dz != 0 || mv.size() >= 3) {
                                specified = true;
                        }
                        dest.push_back(std::move(fm));
                }
                return specified;
};

        std::vector<std::vector<AnimationFrame>> parsed_paths;
        if (anim_json.contains("movement_paths") && anim_json["movement_paths"].is_array()) {
                for (const auto& path_json : anim_json["movement_paths"]) {
                        std::vector<AnimationFrame> path_frames;
                        bool specified = parse_movement_sequence(path_json, path_frames);
                        if (!path_frames.empty()) {
                                parsed_paths.push_back(std::move(path_frames));
                        } else {
                                parsed_paths.emplace_back();
                        }
                        movement_specified = movement_specified || specified;
                }
        }

        std::vector<AnimationFrame> primary_path;
        if (anim_json.contains("movement") && anim_json["movement"].is_array()) {
                bool specified = parse_movement_sequence(anim_json["movement"], primary_path);
                movement_specified = movement_specified || specified;
        }

        if (!primary_path.empty()) {
                parsed_paths.insert(parsed_paths.begin(), std::move(primary_path));
        }

        if (parsed_paths.empty()) {
                parsed_paths.emplace_back();
        }

        if (legacy_movement_children_found) {
                cache_invalid_detected = true;
                vibble::log::warn("[AnimationLoader] " + info.name + "::" + trigger
                          + " contains legacy child data in movement frames; child_timelines is the only supported child format. Continuing with animation load while ignoring child data.");
                // Don't return - continue loading the animation without the child data
        }

        std::vector<std::vector<AnimationFrame>> authored_movement_paths = parsed_paths;
        animation.movement_paths_ = std::move(parsed_paths);
        if (animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        const Animation& src_anim = it->second;
                        if (!src_anim.frames.empty()) {
                                AnimationCloner::Options opts{};
                                opts.flip_horizontal           = animation.flipped_source;
                                opts.flip_vertical             = animation.flip_vertical_source;
                                opts.reverse_frames            = animation.reverse_source;
                                opts.flip_movement_horizontal  = animation.flip_movement_horizontal;
                                opts.flip_movement_vertical    = animation.flip_movement_vertical;

                                if (!AnimationCloner::Clone(src_anim, animation, opts, renderer, info)) {
                                        flush_diagnostics();
                                        return;
                                }
                                reused_animation = true;
                        }
                }
        } else if (animation.source.kind == "folder") {

                const fs::path cache_folder_path = fs::path(root_cache) / trigger;
                std::string cache_folder = cache_folder_path.string();

                std::size_t variant_count = animation.variant_steps_.size();
                if (variant_count == 0) {
                        animation.variant_steps_.push_back(1.0f);
                        variant_count = 1;
                        info.scale_variants = animation.variant_steps_;
                }

                std::vector<VariantLayerPaths> variant_paths;
                variant_paths.reserve(variant_count);
                for (std::size_t idx = 0; idx < variant_count; ++idx) {
                        variant_paths.push_back(build_variant_layer_paths(cache_folder, animation.variant_steps_, idx));
                }

                auto free_surface_lists = [](std::vector<std::vector<SDL_Surface*>>& lists) {
                        for (auto& list : lists) {
                                for (SDL_Surface* surf : list) {
                                        if (surf) {
                                                SDL_FreeSurface(surf);
                                        }
                                }
                                list.clear();
                        }
};

                int frame_count = 0;
                std::size_t working_variant_idx = 0;
                for (std::size_t idx = 0; idx < variant_paths.size(); ++idx) {
                        const fs::path normal_folder_path(variant_paths[idx].normal_folder);
                        const fs::path test_frame = normal_folder_path / "0.png";

                        std::error_code test_ec;
                        if (fs::exists(test_frame, test_ec) && !test_ec) {
                                frame_count = count_png_files(variant_paths[idx].normal_folder);
                                if (frame_count > 0) {
                                        working_variant_idx = idx;
                                        break;
                                }
                        }
                }

                if (frame_count == 0) {
                        flush_diagnostics();
                        return;
                }

                std::vector<std::vector<SDL_Surface*>> variant_surfaces(variant_count);
                std::vector<std::vector<SDL_Surface*>> foreground_surfaces(variant_count);
                std::vector<std::vector<SDL_Surface*>> background_surfaces(variant_count);
                bool all_surfaces_loaded = true;
                for (std::size_t idx = 0; idx < variant_count; ++idx) {
                        const VariantLayerPaths& paths = variant_paths[idx];
                        std::vector<SDL_Surface*> loaded;
                        bool loaded_ok = CacheManager::load_surface_sequence(paths.normal_folder, frame_count, loaded);
                        if (loaded_ok && static_cast<int>(loaded.size()) == frame_count) {
                                variant_surfaces[idx] = std::move(loaded);
                        } else {
                                all_surfaces_loaded = false;
                                std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                                          << " failed to load variant " << idx << " from " << paths.normal_folder << "\n";
                                break;
                        }

                        std::vector<SDL_Surface*> fg_loaded;
                        if (CacheManager::load_surface_sequence(paths.foreground_folder, frame_count, fg_loaded) &&
                            static_cast<int>(fg_loaded.size()) == frame_count) {
                                foreground_surfaces[idx] = std::move(fg_loaded);
                        }

                        std::vector<SDL_Surface*> bg_loaded;
                        if (CacheManager::load_surface_sequence(paths.background_folder, frame_count, bg_loaded) &&
                            static_cast<int>(bg_loaded.size()) == frame_count) {
                                background_surfaces[idx] = std::move(bg_loaded);
                        }

                }

                if (!all_surfaces_loaded || variant_surfaces[0].empty() || !variant_surfaces[0][0]) {
                        std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " cache surfaces not found or incomplete, cannot load animation\n";
                        free_surface_lists(variant_surfaces);
                        free_surface_lists(foreground_surfaces);
                        free_surface_lists(background_surfaces);
                        flush_diagnostics();
                        return;
                }

                const int expected_frames = static_cast<int>(variant_surfaces[0].size());
                std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                          << " loaded " << expected_frames << " cached frame(s) for "
                          << variant_count << " variant(s)\n";

                original_canvas_width  = variant_surfaces[0][0]->w;
                original_canvas_height = variant_surfaces[0][0]->h;
                scaled_sprite_w        = scaled_dimension(variant_surfaces[0][0]->w, safe_scale);
                scaled_sprite_h        = scaled_dimension(variant_surfaces[0][0]->h, safe_scale);

                int orig_w = variant_surfaces[0][0]->w;
                int orig_h = variant_surfaces[0][0]->h;

                if ((scaled_sprite_w <= 0 || scaled_sprite_h <= 0) && orig_w > 0 && orig_h > 0) {
                        int fallback_w = scaled_dimension(orig_w, safe_scale);
                        int fallback_h = scaled_dimension(orig_h, safe_scale);
                        if (fallback_w <= 0) fallback_w = 1;
                        if (fallback_h <= 0) fallback_h = 1;
                        scaled_sprite_w = fallback_w;
                        scaled_sprite_h = fallback_h;
                }

                animation.frames.clear();
                animation.frame_cache_.clear();
                animation.frames.reserve(expected_frames);

                animation.frame_cache_.reserve(expected_frames);

                for (std::size_t frame_idx = 0; frame_idx < variant_surfaces[0].size(); ++frame_idx) {
                        Animation::FrameCache cache_entry;
                        cache_entry.resize(variant_count);
                        for (std::size_t variant_idx = 0; variant_idx < variant_count; ++variant_idx) {
                                SDL_Surface* surface = (frame_idx < variant_surfaces[variant_idx].size()) ? variant_surfaces[variant_idx][frame_idx] : nullptr;
                                SDL_Texture* tex_variant = nullptr;
                                if (surface) {
                                        tex_variant = CacheManager::surface_to_texture(renderer, surface);
                                        if (tex_variant) {
                                                apply_scale_mode(tex_variant, info);
                                        }
                                }
                                int tex_w = surface ? surface->w : 0;
                                int tex_h = surface ? surface->h : 0;
                                if (tex_variant && (tex_w == 0 || tex_h == 0)) {
                                        SDL_QueryTexture(tex_variant, nullptr, nullptr, &tex_w, &tex_h);
                                }
                                cache_entry.textures[variant_idx] = tex_variant;
                                cache_entry.widths[variant_idx]   = tex_w;
                                cache_entry.heights[variant_idx]  = tex_h;

                                SDL_Texture* fg_tex = nullptr;
                                if (frame_idx < foreground_surfaces[variant_idx].size() && foreground_surfaces[variant_idx][frame_idx]) {
                                        fg_tex = CacheManager::surface_to_texture(renderer, foreground_surfaces[variant_idx][frame_idx]);
                                        if (fg_tex) {
                                                apply_scale_mode(fg_tex, info);
                                        }
                                }
                                cache_entry.foreground_textures[variant_idx] = fg_tex;

                                SDL_Texture* bg_tex = nullptr;
                                if (frame_idx < background_surfaces[variant_idx].size() && background_surfaces[variant_idx][frame_idx]) {
                                        bg_tex = CacheManager::surface_to_texture(renderer, background_surfaces[variant_idx][frame_idx]);
                                        if (bg_tex) {
                                                apply_scale_mode(bg_tex, info);
                                        }
                                }
                                cache_entry.background_textures[variant_idx] = bg_tex;

                        }
                        animation.frame_cache_.push_back(std::move(cache_entry));
                }

                free_surface_lists(variant_surfaces);
                free_surface_lists(foreground_surfaces);
                free_surface_lists(background_surfaces);
                if (animation.reverse_source && !animation.frame_cache_.empty()) {
                        std::reverse(animation.frame_cache_.begin(), animation.frame_cache_.end());
                }
                loaded_from_cache = true;
        }

        if (animation.frame_cache_.empty() &&
            animation.source.kind == "animation" &&
            !animation.source.name.empty()) {
                auto src_it = info.animations.find(animation.source.name);
                if (src_it != info.animations.end() && !src_it->second.frame_cache_.empty()) {
                        AnimationCloner::Options opts{};
                        opts.flip_horizontal           = animation.flipped_source;
                        opts.flip_vertical             = animation.flip_vertical_source;
                        opts.reverse_frames            = animation.reverse_source;
                        opts.flip_movement_horizontal  = animation.flip_movement_horizontal;
                        opts.flip_movement_vertical    = animation.flip_movement_vertical;
                        std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " late-cloning from source animation '" << animation.source.name
                                  << "' (flipH=" << opts.flip_horizontal
                                  << ", flipV=" << opts.flip_vertical
                                  << ", flipMoveH=" << opts.flip_movement_horizontal
                                  << ", flipMoveV=" << opts.flip_movement_vertical
                                  << ", reverse=" << opts.reverse_frames << ")\n";
                        if (AnimationCloner::Clone(src_it->second, animation, opts, renderer, info)) {
                                reused_animation = true;
                        }
                }
        }

        auto apply_movement_transforms = [&](std::vector<std::vector<AnimationFrame>>& paths) {
                if (animation.reverse_source) {
                        for (auto& path : paths) {
                                std::reverse(path.begin(), path.end());
                        }
                }
                if (animation.flip_movement_horizontal) {
                        for (auto& path : paths) {
                                for (auto& frame : path) {
                                        frame.dx = -frame.dx;
                                }
                        }
                }
                if (animation.flip_movement_vertical) {
                        for (auto& path : paths) {
                                for (auto& frame : path) {
                                        frame.dy = -frame.dy;
                                }
                        }
                }
};

        const bool derive_from_animation = (animation.source.kind == "animation" && !animation.source.name.empty());
        const bool use_inherited_movement = derive_from_animation && animation.inherit_source_movement;
        bool       movement_from_source = false;
        if (use_inherited_movement) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        animation.movement_paths_ = it->second.movement_paths_;
                        movement_from_source = true;
                } else if (!movement_specified) {

                        animation.movement_paths_.assign(1, {});
                } else {
                        std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " source animation '" << animation.source.name
                                  << "' not available; keeping authored movement\n";
                }
        }
        if (!movement_from_source) {
                animation.movement_paths_ = authored_movement_paths;
        }
        if (derive_from_animation) {
                apply_movement_transforms(animation.movement_paths_);
        }
        auto warn_on_legacy_frame_children = [&](const std::vector<std::vector<AnimationFrame>>& paths) {
                for (const auto& path : paths) {
                        for (const auto& frame : path) {
                                if (!frame.children.empty()) {
                                        cache_invalid_detected = true;
                                        vibble::log::warn("[AnimationLoader] " + info.name + "::" + trigger
                                                  + " contains legacy frame child data; child_timelines is the only supported child format. Continuing with animation load while ignoring child data.");
                                        // Clear legacy child data but continue loading
                                        const_cast<AnimationFrame&>(frame).children.clear();
                                }
                        }
                }
        };
        warn_on_legacy_frame_children(animation.movement_paths_);
        const bool has_audio_json = anim_json.contains("audio") && anim_json["audio"].is_object();
        const nlohmann::json* audio_json = has_audio_json ? &anim_json["audio"] : nullptr;
        auto clamp_volume = [](int value) {
                if (value < 0) return 0;
                if (value > 100) return 100;
                return value;
};
        if (audio_json) {
                animation.audio_clip.volume = clamp_volume(audio_json->value("volume", animation.audio_clip.volume));
                animation.audio_clip.effects = audio_json->value("effects", animation.audio_clip.effects);
                try {
                        std::string clip_name = audio_json->value("name", std::string{});
                        if (!clip_name.empty()) {
                                animation.audio_clip.name = clip_name;
                                std::filesystem::path clip_path = std::filesystem::path(dir_path) / (clip_name + ".wav");
                                animation.audio_clip.path = clip_path.lexically_normal().string();
                                animation.audio_clip.chunk = load_audio_clip(animation.audio_clip.path);
                        }
                } catch (...) {

                }
        }
        if (!animation.audio_clip.chunk && animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        animation.audio_clip = it->second.audio_clip;
                        if (audio_json) {
                                if (audio_json->contains("volume")) {
                                        animation.audio_clip.volume = clamp_volume(audio_json->value("volume", animation.audio_clip.volume));
                                }
                                if (audio_json->contains("effects")) {
                                        animation.audio_clip.effects = audio_json->value("effects", animation.audio_clip.effects);
                                }
                        }
                }
        }
        const std::size_t frame_count = animation.frame_cache_.size();
        if (animation.movement_paths_.empty()) {
                animation.movement_paths_.emplace_back();
        }

        animation.frames.clear();

        bool any_motion = false;
        for (std::size_t path_idx = 0; path_idx < animation.movement_paths_.size(); ++path_idx) {
                auto& path = animation.movement_paths_[path_idx];
                if (path.size() != frame_count) {
                        path.resize(frame_count);
                }
                for (std::size_t i = 0; i < path.size(); ++i) {
                        AnimationFrame& f = path[i];
                        f.prev        = (i > 0) ? &path[i - 1] : nullptr;
                        f.next        = (i + 1 < path.size()) ? &path[i + 1] : nullptr;
                        f.is_first    = (i == 0);
                        f.is_last     = (i + 1 == path.size());
                        f.frame_index = static_cast<int>(i);

                        f.variants.clear();
                        if (i < animation.frame_cache_.size()) {
                            const auto& cache = animation.frame_cache_[i];
                            for (size_t v = 0; v < cache.textures.size(); ++v) {
                                FrameVariant variant;
                                variant.varient = static_cast<int>(v);
                                variant.base_texture = cache.textures[v];
                                if (v < cache.foreground_textures.size()) variant.foreground_texture = cache.foreground_textures[v];
                                if (v < cache.background_textures.size()) variant.background_texture = cache.background_textures[v];
                                f.variants.push_back(variant);
                            }
                        }

                        if (f.dx != 0 || f.dy != 0 || f.dz != 0) {
                                any_motion = true;
                        }

                if (path_idx == 0) {
                        animation.frames.push_back(&f);
                }
        }
}

        apply_combat_geometry(animation.movement_paths_, hit_geometry_json, attack_geometry_json);

        animation.total_dx = 0;
        animation.total_dy = 0;
        animation.total_dz = 0;
        if (!animation.movement_paths_.empty()) {
                const auto& primary = animation.movement_paths_.front();
                for (const auto& frame : primary) {
                        animation.total_dx += frame.dx;
                        animation.total_dy += frame.dy;
                        animation.total_dz += frame.dz;
                        if (frame.dx != 0 || frame.dy != 0 || frame.dz != 0) {
                                any_motion = true;
                        }
                }
        }

        animation.movment = any_motion;
        animation.number_of_frames = static_cast<int>(frame_count);
        const bool requested_child_timelines = anim_json.contains("child_timelines");
        // child_timelines are the sole source of truth for child data; legacy paths are rejected.
        const bool loaded_child_timelines = AnimationLoader::load_child_timelines_from_json(anim_json, animation);
        if (!loaded_child_timelines) {
                if (requested_child_timelines) {
                        cache_invalid_detected = true;
                        std::cerr << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " failed to load child_timelines; legacy child formats are not supported.\n";
                }
                animation.child_timelines().clear();
        }
        animation.rebuild_frames_from_child_timelines();
        animation.refresh_child_start_events();
        if (trigger == "default" && !animation.frames.empty() && !animation.frames[0]->variants.empty()) {
                base_sprite = animation.frames[0]->variants[0].base_texture;
                info.preview_texture = animation.frames[0]->variants[0].base_texture;
        }

        if (!animation.frames.empty() && !animation.frames[0]->variants.empty()) {
            animation.preview_texture = animation.frames[0]->variants[0].base_texture;
        } else {
            animation.preview_texture = nullptr;
        }

        int frame_width  = 0;
        int frame_height = 0;
        if (!animation.frame_cache_.empty()) {
                frame_width  = animation.frame_cache_[0].widths[0];
                frame_height = animation.frame_cache_[0].heights[0];
                if ((frame_width <= 0 || frame_height <= 0) && animation.frame_cache_[0].textures[0]) {
                        SDL_QueryTexture(animation.frame_cache_[0].textures[0], nullptr, nullptr, &frame_width, &frame_height);
                }
        }

        const auto load_end        = std::chrono::steady_clock::now();
        const double elapsed_secs  = std::chrono::duration<double>(load_end - load_start).count();
        std::string   origin_label = loaded_from_cache ? "cache" : "source";
        if (reused_animation) {
                origin_label = "animation '" + animation.source.name + "'";
        }

        // Load completed

        resolve_inherited_movements(info);
        flush_diagnostics();
}

bool AnimationLoader::load_child_timelines_from_json(const nlohmann::json& anim_json,
                                                     Animation& animation) {
                auto timelines_node = anim_json.find("child_timelines");
                if (timelines_node == anim_json.end() || !timelines_node->is_array()) {
                        return false;
                }

                std::vector<std::string> child_assets = animation.child_assets();
                std::unordered_map<std::string, int> child_lookup;
                child_lookup.reserve(child_assets.size());
                for (std::size_t i = 0; i < child_assets.size(); ++i) {
                        child_lookup.emplace(child_assets[i], static_cast<int>(i));
                }

                const auto& existing_timelines = animation.child_timelines();
                std::unordered_map<std::string, const AnimationChildData*> previous_by_asset;
                previous_by_asset.reserve(existing_timelines.size());
                for (const auto& descriptor : existing_timelines) {
                        if (!descriptor.asset_name.empty()) {
                                previous_by_asset.emplace(descriptor.asset_name, &descriptor);
                        }
                }

                auto note_legacy_child_payload = [&](const std::string& asset_name, const std::string& detail) {
                        std::cout << "[AnimationLoader] child timeline for asset '" << asset_name
                                  << "' contains unsupported legacy child data (" << detail
                                  << "). child_timelines is the only source of truth.\n";
                };
                auto is_legacy_child_frame = [](const nlohmann::json& node) {
                        return !node.is_object() || node.contains("render_in_front");
                };

                auto resolve_child_index = [&](const nlohmann::json& node) -> int {
                        int idx = -1;
                        if (node.contains("child") && node["child"].is_number_integer()) {
                                idx = node["child"].get<int>();
                        } else if (node.contains("child_index") && node["child_index"].is_number_integer()) {
                                idx = node["child_index"].get<int>();
                        }
                        if (idx >= 0 && static_cast<std::size_t>(idx) < child_assets.size()) {
                                return idx;
                        }
                        if (node.contains("asset") && node["asset"].is_string()) {
                                std::string name = node["asset"].get<std::string>();
                                if (name.empty()) {
                                        return -1;
                                }
                                auto lookup = child_lookup.find(name);
                                if (lookup != child_lookup.end()) {
                                        return lookup->second;
                                }
                                child_assets.push_back(name);
                                int new_index = static_cast<int>(child_assets.size() - 1);
                                child_lookup.emplace(name, new_index);
                                return new_index;
                        }
                        return -1;
};

                struct StartMetadata {
                        float time_seconds = 0.0f;
                        int frame_offset = 0;
                        bool present = false;
                };

                auto parse_start_metadata = [&](const nlohmann::json& entry) -> StartMetadata {
                        StartMetadata meta{};
                        auto coerce_float = [](const nlohmann::json& value, float fallback) -> float {
                                if (value.is_number()) {
                                        try {
                                                return static_cast<float>(value.get<double>());
                                        } catch (...) {
                                        }
                                } else if (value.is_string()) {
                                        try {
                                                return std::stof(value.get<std::string>());
                                        } catch (...) {
                                        }
                                }
                                return fallback;
                        };
                        auto coerce_int = [](const nlohmann::json& value, int fallback) -> int {
                                if (value.is_number_integer()) {
                                        try {
                                                return value.get<int>();
                                        } catch (...) {
                                        }
                                } else if (value.is_number()) {
                                        try {
                                                return static_cast<int>(value.get<double>());
                                        } catch (...) {
                                        }
                                } else if (value.is_string()) {
                                        try {
                                                return std::stoi(value.get<std::string>());
                                        } catch (...) {
                                        }
                                }
                                return fallback;
                        };
                        if (entry.contains("start_time")) {
                                meta.time_seconds = coerce_float(entry["start_time"], 0.0f);
                                meta.present = true;
                        }
                        if (entry.contains("start_frame")) {
                                meta.frame_offset = coerce_int(entry["start_frame"], 0);
                                meta.present = true;
                                if (!entry.contains("start_time")) {
                                        meta.time_seconds = static_cast<float>(meta.frame_offset) / static_cast<float>(kBaseAnimationFps);
                                }
                        } else if (entry.contains("start") && entry["start"].is_number()) {
                                meta.frame_offset = coerce_int(entry["start"], 0);
                                meta.present = true;
                                if (!entry.contains("start_time")) {
                                        meta.time_seconds = static_cast<float>(meta.frame_offset) / static_cast<float>(kBaseAnimationFps);
                                }
                        } else if (meta.present && meta.frame_offset == 0) {
                                meta.frame_offset = static_cast<int>(std::lround(meta.time_seconds * static_cast<float>(kBaseAnimationFps)));
                        }
                        return meta;
                };

                std::unordered_map<int, AnimationChildData> parsed;
                parsed.reserve(timelines_node->size());
                bool fatal_error = false;

                for (const auto& entry : *timelines_node) {
                        if (!entry.is_object()) {
                                continue;
                        }
                        const int child_idx = resolve_child_index(entry);
                        if (child_idx < 0) {
                                std::cout << "[AnimationLoader] child timeline entry missing valid child index.\n";
                                fatal_error = true;
                                continue;
                        }

                        std::string asset_name = (static_cast<std::size_t>(child_idx) < child_assets.size())
                                                        ? child_assets[static_cast<std::size_t>(child_idx)]
                                                        : std::string{};
                        if (entry.contains("render_in_front") || entry.contains("children")) {
                                note_legacy_child_payload(asset_name, "legacy child fields in child_timeline entry");
                                fatal_error = true;
                                continue;
                        }

                        AnimationChildData timeline;
                        timeline.asset_name = asset_name;
                        timeline.animation_override = entry.value("animation", std::string{});
                        const auto mode = parse_child_mode(entry);
                        if (!mode) {
                                std::cout << "[AnimationLoader] child timeline for asset '" << timeline.asset_name
                                          << "' omitted required mode (static|async).\n";
                                fatal_error = true;
                                continue;
                        }
                        timeline.mode = *mode;
                        timeline.auto_start = entry.value("auto_start", entry.value("autostart", false));
                        const StartMetadata start_meta = parse_start_metadata(entry);
                        timeline.has_start_time = start_meta.present;
                        timeline.start_time = start_meta.time_seconds;
                        timeline.start_frame = start_meta.present
                                                   ? start_meta.frame_offset
                                                   : static_cast<int>(std::lround(timeline.start_time * static_cast<float>(kBaseAnimationFps)));
                        if (timeline.has_start_time && !timeline.auto_start) {
                                timeline.auto_start = true;
                        }
                        const auto frames_it = entry.find("frames");
                        if (frames_it != entry.end()) {
                                if (!frames_it->is_array()) {
                                        note_legacy_child_payload(timeline.asset_name, "frames must be an array of objects");
                                        fatal_error = true;
                                        continue;
                                }
                                for (const auto& sample : *frames_it) {
                                        if (is_legacy_child_frame(sample)) {
                                                note_legacy_child_payload(timeline.asset_name, "legacy child frame payload");
                                                fatal_error = true;
                                                break;
                                        }
                                        const auto parsed_frame = parse_child_frame_sample(sample, child_idx, timeline.asset_name);
                                        if (!parsed_frame) {
                                                fatal_error = true;
                                                break;
                                        }
                                        timeline.frames.push_back(*parsed_frame);
                                }
                                if (fatal_error) {
                                        continue;
                                }
                        }
                        parsed[child_idx] = std::move(timeline);
                }

                if (child_assets.empty() || fatal_error) {
                        return false;
                }

                const std::size_t parent_frame_count = animation.frames.size();
                auto make_default_sample = [&](int idx) {
                        return make_default_child_frame(idx);
};

                std::vector<AnimationChildData> descriptors;
                descriptors.reserve(child_assets.size());

                for (std::size_t idx = 0; idx < child_assets.size(); ++idx) {
                        const int child_index = static_cast<int>(idx);
                        const auto parsed_it = parsed.find(child_index);
                        const AnimationChildData* parsed_data = (parsed_it != parsed.end()) ? &parsed_it->second : nullptr;
                        const auto prev_it = previous_by_asset.find(child_assets[idx]);
                        const AnimationChildData* previous = (prev_it != previous_by_asset.end()) ? prev_it->second : nullptr;

                        if (!parsed_data && !previous) {
                                std::cout << "[AnimationLoader] child timeline for asset '" << child_assets[idx]
                                          << "' is missing configuration and cannot be inferred.\n";
                                fatal_error = true;
                                break;
                        }

                        AnimationChildData descriptor;
                        descriptor.asset_name = child_assets[idx];
                        descriptor.name = previous ? previous->name : std::string{};
                        descriptor.animation_override = parsed_data ? parsed_data->animation_override
                                                                    : (previous ? previous->animation_override : std::string{});
                        descriptor.mode = parsed_data ? parsed_data->mode : previous->mode;
                        descriptor.auto_start = parsed_data ? parsed_data->auto_start
                                                            : (previous ? previous->auto_start : (descriptor.mode == AnimationChildMode::Static));
                        descriptor.start_time = parsed_data ? parsed_data->start_time
                                                            : (previous ? previous->start_time : 0.0f);
                        descriptor.start_frame = parsed_data ? parsed_data->start_frame
                                                             : (previous ? previous->start_frame : 0);
                        descriptor.has_start_time = parsed_data ? parsed_data->has_start_time
                                                                 : (previous ? previous->has_start_time : false);
                        if (descriptor.has_start_time && descriptor.start_frame <= 0) {
                                descriptor.start_frame = static_cast<int>(std::lround(descriptor.start_time * static_cast<float>(kBaseAnimationFps)));
                        } else if (descriptor.start_frame > 0 && !descriptor.has_start_time) {
                                descriptor.start_time = static_cast<float>(descriptor.start_frame) / static_cast<float>(kBaseAnimationFps);
                                descriptor.has_start_time = true;
                        }

                        if (descriptor.mode == AnimationChildMode::Static) {
                                const std::size_t sample_count = (parent_frame_count > 0) ? parent_frame_count : ((previous && previous->is_static() && !previous->frames.empty()) ? previous->frames.size() : static_cast<std::size_t>(1));
                                descriptor.frames.assign(sample_count, make_default_sample(child_index));

                                if (parsed_data && !parsed_data->frames.empty()) {
                                        const std::size_t limit = std::min(parsed_data->frames.size(), descriptor.frames.size());
                                        for (std::size_t frame_idx = 0; frame_idx < limit; ++frame_idx) {
                                                descriptor.frames[frame_idx] = parsed_data->frames[frame_idx];
                                                descriptor.frames[frame_idx].child_index = child_index;
                                        }
                                } else if (previous && previous->is_static()) {
                                        const std::size_t limit = std::min(previous->frames.size(), descriptor.frames.size());
                                        for (std::size_t frame_idx = 0; frame_idx < limit; ++frame_idx) {
                                                descriptor.frames[frame_idx] = previous->frames[frame_idx];
                                                descriptor.frames[frame_idx].child_index = child_index;
                                        }
                                }
                        } else {
                                if (parsed_data && !parsed_data->frames.empty()) {
                                        descriptor.frames = parsed_data->frames;
                                        for (auto& sample : descriptor.frames) {
                                                sample.child_index = child_index;
                                        }
                                } else if (previous && previous->is_async() && !previous->frames.empty()) {
                                        descriptor.frames = previous->frames;
                                        for (auto& sample : descriptor.frames) {
                                                sample.child_index = child_index;
                                        }
                                } else {
                                        descriptor.frames.push_back(make_default_sample(child_index));
                                }
                        }

                        descriptors.push_back(std::move(descriptor));
                }

                if (fatal_error) {
                        return false;
                }

                animation.child_assets() = std::move(child_assets);
                animation.child_timelines() = std::move(descriptors);
                return true;
        }

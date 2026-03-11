#include "animation_loader.hpp"
#include "animation.hpp"
#include "animation_cloner.hpp"
#include "anchor_point.hpp"
#include "asset_info.hpp"
#include "asset_types.hpp"
#include "json_coercion.hpp"
#include "surface_utils.hpp"
#include "utils/cache_manager.hpp"
#include "rendering/render/render.hpp"
#include "utils/loading_status_notifier.hpp"
#include "utils/log.hpp"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace {

using json_coercion::read_bool_field_like;
using json_coercion::read_int_field_like;
using json_coercion::read_string_field_like;

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

DisplacedAssetAnchorPoint read_anchor_point(const nlohmann::json& node, bool& valid) {
        DisplacedAssetAnchorPoint anchor{};
        valid = false;
        if (!node.is_object()) {
                return anchor;
        }
        anchor.name = read_string_field_like(node, "name", std::string{});
        if (anchor.name.empty()) {
                return anchor;
        }

        if (!node.contains("texture_x") || !node["texture_x"].is_number_integer()) {
                return anchor;
        }
        if (!node.contains("texture_y") || !node["texture_y"].is_number_integer()) {
                return anchor;
        }
        if (!node.contains("depth_offset") || !node["depth_offset"].is_number_integer()) {
                return anchor;
        }

        anchor.texture_x = node["texture_x"].get<int>();
        anchor.texture_y = node["texture_y"].get<int>();
        anchor.depth_offset = node["depth_offset"].get<int>();

        valid = anchor.is_valid();
        return anchor;
}

std::vector<std::vector<DisplacedAssetAnchorPoint>> parse_anchor_frames(const nlohmann::json& anchor_json,
                                                                        std::size_t           frame_count) {
        std::vector<std::vector<DisplacedAssetAnchorPoint>> anchors(frame_count);
        if (!anchor_json.is_array()) {
                return anchors;
        }
        const std::size_t limit = std::min<std::size_t>(frame_count, anchor_json.size());
        for (std::size_t idx = 0; idx < limit; ++idx) {
                const auto& entry = anchor_json[idx];
                if (!entry.is_array()) continue;
                std::unordered_set<std::string> names;
                for (const auto& node : entry) {
                        bool ok = false;
                        auto anchor = read_anchor_point(node, ok);
                        if (!ok) continue;
                        if (names.insert(anchor.name).second) {
                                anchors[idx].push_back(anchor);
                        }
                }
        }
        if (anchors.size() < frame_count) {
                anchors.resize(frame_count);
        }
        return anchors;
}

std::vector<std::vector<DisplacedAssetAnchorPoint>> collect_anchor_frames_from_animation(const Animation& anim,
                                                                                        std::size_t       frame_count) {
        std::vector<std::vector<DisplacedAssetAnchorPoint>> anchors(frame_count);
        if (anim.movement_path_count() == 0) {
                return anchors;
        }
        const auto& path = anim.movement_path(0);
        const std::size_t limit = std::min(frame_count, path.size());
        for (std::size_t i = 0; i < limit; ++i) {
                anchors[i] = path[i].anchor_points;
        }
        return anchors;
}

void apply_anchor_transforms(std::vector<std::vector<DisplacedAssetAnchorPoint>>& anchors,
                             const std::vector<Animation::FrameCache>& frame_cache,
                             bool                                                 reverse_frames,
                             bool                                                 flip_x,
                             bool                                                 flip_y,
                             bool                                                 flip_movement_x,
                             bool                                                 flip_movement_y) {
        if (anchors.empty()) {
                return;
        }
        if (reverse_frames) {
                std::reverse(anchors.begin(), anchors.end());
        }
        const bool flip_horizontal = flip_x || flip_movement_x;
        const bool flip_vertical = flip_y || flip_movement_y;
        for (std::size_t frame_index = 0; frame_index < anchors.size(); ++frame_index) {
                auto& frame = anchors[frame_index];
                int frame_w = 0;
                int frame_h = 0;
                if (frame_index < frame_cache.size()) {
                        const auto& cache = frame_cache[frame_index];
                        if (!cache.widths.empty()) frame_w = cache.widths.front();
                        if (!cache.heights.empty()) frame_h = cache.heights.front();
                        if (!cache.source_rects.empty() && !cache.uses_atlas.empty() && cache.uses_atlas.front()) {
                                frame_w = cache.source_rects.front().w;
                                frame_h = cache.source_rects.front().h;
                        }
                }
                for (auto& anchor : frame) {
                        if (flip_horizontal && frame_w > 0) {
                                anchor.texture_x = frame_w - 1 - anchor.texture_x;
                        }
                        if (flip_vertical && frame_h > 0) {
                                anchor.texture_y = frame_h - 1 - anchor.texture_y;
                        }
                }
        }
}

std::string make_unique_name(const std::string& desired,
                             const std::string& fallback_prefix,
                             std::unordered_set<std::string>& used_names,
                             std::size_t ordinal) {
        std::string base = desired;
        if (base.empty()) {
                base = fallback_prefix + "_" + std::to_string(ordinal + 1);
        }
        std::string candidate = base;
        int suffix = 2;
        while (!used_names.insert(candidate).second) {
                candidate = base + "_" + std::to_string(suffix++);
        }
        return candidate;
}

template <typename TBox>
void parse_box_corners(TBox& box, const nlohmann::json& node) {
        if (!node.is_object()) {
                return;
        }
        const nlohmann::json& corners = (node.contains("corners") && node["corners"].is_array())
                                            ? node["corners"]
                                            : nlohmann::json::array();
        for (std::size_t idx = 0; idx < box.corners.size(); ++idx) {
                animation_update::FrameBoxCorner corner{};
                if (idx < corners.size() && corners[idx].is_object()) {
                        const auto& corner_node = corners[idx];
                        corner.texture_x = std::max(0, read_int_field_like(corner_node, "texture_x", 0));
                        corner.texture_y = std::max(0, read_int_field_like(corner_node, "texture_y", 0));
                }
                box.corners[idx] = corner;
        }
}

animation_update::FrameHitBox parse_hit_box(const nlohmann::json& node, std::size_t ordinal) {
        animation_update::FrameHitBox box{};
        if (!node.is_object()) {
                return box;
        }
        box.name = read_string_field_like(node, "name", std::string{"hit_box_" + std::to_string(ordinal + 1)});
        box.extrusion_amount = std::max(0, read_int_field_like(node, "extrusion_amount", 0));
        parse_box_corners(box, node);
        return box;
}

animation_update::FrameAttackBox parse_attack_box(const nlohmann::json& node, std::size_t ordinal) {
        animation_update::FrameAttackBox box{};
        if (!node.is_object()) {
                return box;
        }
        box.name = read_string_field_like(node, "name", std::string{"attack_box_" + std::to_string(ordinal + 1)});
        box.extrusion_amount = std::max(0, read_int_field_like(node, "extrusion_amount", 0));
        box.damage_amount = read_int_field_like(node, "damage_amount", read_int_field_like(node, "damage", 0));
        parse_box_corners(box, node);
        return box;
}

std::vector<animation_update::FrameHitBox> parse_hit_box_frame(const nlohmann::json& entry) {
        std::vector<animation_update::FrameHitBox> boxes;
        if (!entry.is_array()) {
                return boxes;
        }
        std::unordered_set<std::string> used_names;
        boxes.reserve(entry.size());
        for (std::size_t idx = 0; idx < entry.size(); ++idx) {
                if (!entry[idx].is_object()) {
                        continue;
                }
                animation_update::FrameHitBox box = parse_hit_box(entry[idx], idx);
                box.name = make_unique_name(box.name, "hit_box", used_names, idx);
                if (box.is_valid()) {
                        boxes.push_back(std::move(box));
                }
        }
        return boxes;
}

std::vector<animation_update::FrameAttackBox> parse_attack_box_frame(const nlohmann::json& entry) {
        std::vector<animation_update::FrameAttackBox> boxes;
        if (!entry.is_array()) {
                return boxes;
        }
        std::unordered_set<std::string> used_names;
        boxes.reserve(entry.size());
        for (std::size_t idx = 0; idx < entry.size(); ++idx) {
                if (!entry[idx].is_object()) {
                        continue;
                }
                animation_update::FrameAttackBox box = parse_attack_box(entry[idx], idx);
                box.name = make_unique_name(box.name, "attack_box", used_names, idx);
                if (box.is_valid()) {
                        boxes.push_back(std::move(box));
                }
        }
        return boxes;
}

std::vector<std::vector<animation_update::FrameHitBox>> parse_hit_box_frames(const nlohmann::json& hit_boxes_json,
                                                                              std::size_t frame_count) {
        std::vector<std::vector<animation_update::FrameHitBox>> result(frame_count);
        if (!hit_boxes_json.is_array()) {
                return result;
        }
        const std::size_t limit = std::min<std::size_t>(frame_count, hit_boxes_json.size());
        for (std::size_t frame_idx = 0; frame_idx < limit; ++frame_idx) {
                result[frame_idx] = parse_hit_box_frame(hit_boxes_json[frame_idx]);
        }
        return result;
}

std::vector<std::vector<animation_update::FrameAttackBox>> parse_attack_box_frames(const nlohmann::json& attack_boxes_json,
                                                                                    std::size_t frame_count) {
        std::vector<std::vector<animation_update::FrameAttackBox>> result(frame_count);
        if (!attack_boxes_json.is_array()) {
                return result;
        }
        const std::size_t limit = std::min<std::size_t>(frame_count, attack_boxes_json.size());
        for (std::size_t frame_idx = 0; frame_idx < limit; ++frame_idx) {
                result[frame_idx] = parse_attack_box_frame(attack_boxes_json[frame_idx]);
        }
        return result;
}

std::vector<std::vector<animation_update::FrameHitBox>> collect_hit_box_frames_from_animation(const Animation& anim,
                                                                                               std::size_t frame_count) {
        std::vector<std::vector<animation_update::FrameHitBox>> out(frame_count);
        if (anim.movement_path_count() == 0) {
                return out;
        }
        const auto& path = anim.movement_path(0);
        const std::size_t limit = std::min(frame_count, path.size());
        for (std::size_t i = 0; i < limit; ++i) {
                out[i].assign(path[i].hit_boxes.boxes.begin(), path[i].hit_boxes.boxes.end());
        }
        return out;
}

std::vector<std::vector<animation_update::FrameAttackBox>> collect_attack_box_frames_from_animation(const Animation& anim,
                                                                                                     std::size_t frame_count) {
        std::vector<std::vector<animation_update::FrameAttackBox>> out(frame_count);
        if (anim.movement_path_count() == 0) {
                return out;
        }
        const auto& path = anim.movement_path(0);
        const std::size_t limit = std::min(frame_count, path.size());
        for (std::size_t i = 0; i < limit; ++i) {
                out[i].assign(path[i].attack_boxes.boxes.begin(), path[i].attack_boxes.boxes.end());
        }
        return out;
}

template <typename TBox>
void apply_box_transforms(std::vector<std::vector<TBox>>& boxes,
                          const std::vector<Animation::FrameCache>& frame_cache,
                          bool reverse_frames,
                          bool flip_x,
                          bool flip_y,
                          bool flip_movement_x,
                          bool flip_movement_y) {
        if (boxes.empty()) {
                return;
        }
        if (reverse_frames) {
                std::reverse(boxes.begin(), boxes.end());
        }
        const bool flip_horizontal = flip_x || flip_movement_x;
        const bool flip_vertical = flip_y || flip_movement_y;
        for (std::size_t frame_index = 0; frame_index < boxes.size(); ++frame_index) {
                int frame_w = 0;
                int frame_h = 0;
                if (frame_index < frame_cache.size()) {
                        const auto& cache = frame_cache[frame_index];
                        if (!cache.widths.empty()) frame_w = cache.widths.front();
                        if (!cache.heights.empty()) frame_h = cache.heights.front();
                        if (!cache.source_rects.empty() && !cache.uses_atlas.empty() && cache.uses_atlas.front()) {
                                frame_w = cache.source_rects.front().w;
                                frame_h = cache.source_rects.front().h;
                        }
                }
                for (auto& box : boxes[frame_index]) {
                        for (auto& corner : box.corners) {
                                if (flip_horizontal && frame_w > 0) {
                                        corner.texture_x = frame_w - 1 - corner.texture_x;
                                }
                                if (flip_vertical && frame_h > 0) {
                                        corner.texture_y = frame_h - 1 - corner.texture_y;
                                }
                        }
                }
        }
}

void apply_frame_boxes(std::vector<std::vector<AnimationFrame>>& paths,
                       const std::vector<std::vector<animation_update::FrameHitBox>>& hit_boxes,
                       const std::vector<std::vector<animation_update::FrameAttackBox>>& attack_boxes) {
        for (auto& path : paths) {
                for (std::size_t idx = 0; idx < path.size(); ++idx) {
                        AnimationFrame& frame = path[idx];
                        if (idx < hit_boxes.size()) {
                                frame.set_hit_boxes(hit_boxes[idx]);
                        } else {
                                frame.set_hit_boxes({});
                        }
                        if (idx < attack_boxes.size()) {
                                frame.set_attack_boxes(attack_boxes[idx]);
                        } else {
                                frame.set_attack_boxes({});
                        }
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

using AudioCache = std::unordered_map<std::string, std::weak_ptr<Animation::AudioClip::AudioBuffer>>;

AudioCache& get_audio_cache() {
        static AudioCache cache;
        return cache;
}

std::shared_ptr<Animation::AudioClip::AudioBuffer> load_audio_clip(const std::string& path) {
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
        SDL_AudioSpec spec{};
        Uint8* audio_data = nullptr;
        Uint32 audio_len = 0;
        if (!SDL_LoadWAV(path.c_str(), &spec, &audio_data, &audio_len)) {
                std::cerr << "[Animation] Failed to load audio '" << path << "': " << SDL_GetError() << "\n";
                return {};
        }
        auto clip = std::make_shared<Animation::AudioClip::AudioBuffer>();
        clip->spec = spec;
        clip->samples.assign(audio_data, audio_data + audio_len);
        SDL_free(audio_data);
        cache[path] = clip;
        return clip;
}

#if SDL_VERSION_ATLEAST(2,0,12)
void apply_scale_mode(SDL_Texture* tex, const AssetInfo& info) {
        if (tex) {
                SDL_SetTextureScaleMode(tex, info.smooth_scaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
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
                     LoadDiagnostics* diagnostics,
                     PrebuiltAnimationFrames* prebuilt_frames)
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
        const Animation* source_animation_ptr = nullptr;
        animation.clear_texture_cache();
        animation.variant_steps_ = info.scale_variants;
        (void)root_cache;

        if (animation.variant_steps_.empty()) {
                render_pipeline::ScalingLogic::NormalizeVariantSteps(animation.variant_steps_);
        }

        if (anim_json.contains("source")) {
                const auto& s = anim_json["source"];
                try {
                        animation.source.kind = read_string_field_like(s, "kind", std::string{"folder"});
                } catch (...) { animation.source.kind = "folder"; }
                try {
                        animation.source.path = read_string_field_like(s, "path", std::string{});
                } catch (...) { animation.source.path.clear(); }
                try {
                        animation.source.name = read_string_field_like(s, "name", std::string{});
                } catch (...) { animation.source.name.clear(); }
        }

        if (animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        source_animation_ptr = &it->second;
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

        animation.flipped_source = read_bool_field_like(anim_json, "flipped_source", false);
        animation.flip_vertical_source = read_bool_field_like(anim_json, "flip_vertical_source", false);
        animation.flip_movement_horizontal = read_bool_field_like(anim_json, "flip_movement_horizontal", false);
        animation.flip_movement_vertical = read_bool_field_like(anim_json, "flip_movement_vertical", false);
        animation.reverse_source = read_bool_field_like(anim_json, "reverse_source", false);
        const bool inherit_source_movement =
            read_bool_field_like(anim_json, "inherit_source_movement", (animation.source.kind == "animation"));
        if (animation.source.kind == "animation" && anim_json.contains("derived_modifiers") &&
            anim_json["derived_modifiers"].is_object()) {
                const auto& modifiers = anim_json["derived_modifiers"];
                animation.reverse_source = read_bool_field_like(modifiers, "reverse", animation.reverse_source);
                animation.flipped_source = read_bool_field_like(modifiers, "flipX", animation.flipped_source);
                animation.flip_vertical_source = read_bool_field_like(modifiers, "flipY", animation.flip_vertical_source);
                animation.flip_movement_horizontal =
                    read_bool_field_like(modifiers, "flipMovementX", animation.flip_movement_horizontal);
                animation.flip_movement_vertical =
                    read_bool_field_like(modifiers, "flipMovementY", animation.flip_movement_vertical);
        } else if (animation.source.kind != "animation") {
                animation.flip_vertical_source = false;
                animation.flip_movement_horizontal = false;
                animation.flip_movement_vertical = false;
        }
        animation.inherit_source_movement = (animation.source.kind == "animation") && inherit_source_movement;

        animation.locked = read_bool_field_like(anim_json, "locked", false);
        animation.loop = read_bool_field_like(anim_json, "loop", true);
        animation.randomize = read_bool_field_like(anim_json, "randomize", false);
        animation.rnd_start = read_bool_field_like(anim_json, "rnd_start", false);
        animation.on_end_animation = read_string_field_like(anim_json, "on_end", std::string{"default"});
        animation.on_end_behavior = Animation::classify_on_end(animation.on_end_animation);
        animation.total_dx = 0;
        animation.total_dy = 0;
        animation.total_dz = 0;
        animation.movement_paths_.clear();
        animation.audio_clip = Animation::AudioClip{};
        bool movement_specified = false;
        nlohmann::json anchor_points_json = nlohmann::json::array();
        const bool has_anchor_points_json = anim_json.contains("anchor_points") && anim_json["anchor_points"].is_array();
        if (has_anchor_points_json) {
                anchor_points_json = anim_json["anchor_points"];
        }
        nlohmann::json hit_boxes_json = nlohmann::json::array();
        const bool has_hit_boxes_json = anim_json.contains("hit_boxes") && anim_json["hit_boxes"].is_array();
        if (has_hit_boxes_json) {
                hit_boxes_json = anim_json["hit_boxes"];
        }
        nlohmann::json attack_boxes_json = nlohmann::json::array();
        const bool has_attack_boxes_json = anim_json.contains("attack_boxes") && anim_json["attack_boxes"].is_array();
        if (has_attack_boxes_json) {
                attack_boxes_json = anim_json["attack_boxes"];
        }

        auto parse_movement_sequence = [&](const nlohmann::json& seq, std::vector<AnimationFrame>& dest) {
                bool specified = false;
                if (!seq.is_array()) return specified;
                auto clamp = [](int v) { return (v < 0) ? 0 : (v > 255 ? 255 : v); };
                for (const auto& mv : seq) {
                        AnimationFrame fm;

                                if (mv.is_object()) {
                                        // Movement uses integer world pixels
                                        fm.dx = read_int_field_like(mv, "x", 0);
                                        fm.dy = read_int_field_like(mv, "y", 0);
                                        fm.dz = read_int_field_like(mv, "z", 0);
                                        fm.z_resort = read_bool_field_like(mv, "resort_z", false);
                                        if (fm.dx != 0 || fm.dy != 0 || fm.dz != 0 || mv.contains("resort_z")) specified = true;
                                        dest.push_back(std::move(fm));
                                        continue;
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
                if (prebuilt_frames && !prebuilt_frames->frames.empty()) {
                        animation.variant_steps_ = prebuilt_frames->variant_steps;
                        animation.frame_cache_ = std::move(prebuilt_frames->frames);
                        info.scale_variants = animation.variant_steps_;
                        original_canvas_width = prebuilt_frames->canvas_width;
                        original_canvas_height = prebuilt_frames->canvas_height;
                        scaled_sprite_w = prebuilt_frames->canvas_width;
                        scaled_sprite_h = prebuilt_frames->canvas_height;
                        loaded_from_cache = true;
                } else {
                        cache_invalid_detected = true;
                        std::cerr << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " missing bundle frames; aborting animation load.\n";
                        flush_diagnostics();
                        return;
                }
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
        const bool has_audio_json = anim_json.contains("audio") && anim_json["audio"].is_object();
        const nlohmann::json* audio_json = has_audio_json ? &anim_json["audio"] : nullptr;
        auto clamp_volume = [](int value) {
                if (value < 0) return 0;
                if (value > 100) return 100;
                return value;
};
        if (audio_json) {
                animation.audio_clip.volume = clamp_volume(read_int_field_like(*audio_json, "volume", animation.audio_clip.volume));
                animation.audio_clip.effects = read_bool_field_like(*audio_json, "effects", animation.audio_clip.effects);
                try {
                        std::string clip_name = read_string_field_like(*audio_json, "name", std::string{});
                        if (!clip_name.empty()) {
                                animation.audio_clip.name = clip_name;
                                std::filesystem::path clip_path = std::filesystem::path(dir_path) / (clip_name + ".wav");
                                animation.audio_clip.path = clip_path.lexically_normal().string();
                                animation.audio_clip.buffer = load_audio_clip(animation.audio_clip.path);
                        }
                } catch (...) {

                }
        }
        if (!animation.audio_clip.buffer && animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        animation.audio_clip = it->second.audio_clip;
                        if (audio_json) {
                                if (audio_json->contains("volume")) {
                                        animation.audio_clip.volume =
                                            clamp_volume(read_int_field_like(*audio_json, "volume", animation.audio_clip.volume));
                                }
                                if (audio_json->contains("effects")) {
                                        animation.audio_clip.effects =
                                            read_bool_field_like(*audio_json, "effects", animation.audio_clip.effects);
                                }
                        }
                }
        }
        const std::size_t frame_count = animation.frame_cache_.size();
        std::vector<std::vector<DisplacedAssetAnchorPoint>> anchor_frames;
        if (has_anchor_points_json) {
                anchor_frames = parse_anchor_frames(anchor_points_json, frame_count);
        } else if (source_animation_ptr) {
                anchor_frames = collect_anchor_frames_from_animation(*source_animation_ptr, frame_count);
                apply_anchor_transforms(anchor_frames,
                                        animation.frame_cache_,
                                        animation.reverse_source,
                                        animation.flipped_source,
                                        animation.flip_vertical_source,
                                        animation.flip_movement_horizontal,
                                        animation.flip_movement_vertical);
        }
        std::vector<std::vector<animation_update::FrameHitBox>> hit_box_frames;
        if (has_hit_boxes_json) {
                hit_box_frames = parse_hit_box_frames(hit_boxes_json, frame_count);
        } else if (source_animation_ptr) {
                hit_box_frames = collect_hit_box_frames_from_animation(*source_animation_ptr, frame_count);
                apply_box_transforms(hit_box_frames,
                                     animation.frame_cache_,
                                     animation.reverse_source,
                                     animation.flipped_source,
                                     animation.flip_vertical_source,
                                     animation.flip_movement_horizontal,
                                     animation.flip_movement_vertical);
        }

        std::vector<std::vector<animation_update::FrameAttackBox>> attack_box_frames;
        if (has_attack_boxes_json) {
                attack_box_frames = parse_attack_box_frames(attack_boxes_json, frame_count);
        } else if (source_animation_ptr) {
                attack_box_frames = collect_attack_box_frames_from_animation(*source_animation_ptr, frame_count);
                apply_box_transforms(attack_box_frames,
                                     animation.frame_cache_,
                                     animation.reverse_source,
                                     animation.flipped_source,
                                     animation.flip_vertical_source,
                                     animation.flip_movement_horizontal,
                                     animation.flip_movement_vertical);
        }
        if (anchor_frames.size() < frame_count) {
                anchor_frames.resize(frame_count);
        }
        if (hit_box_frames.size() < frame_count) {
                hit_box_frames.resize(frame_count);
        }
        if (attack_box_frames.size() < frame_count) {
                attack_box_frames.resize(frame_count);
        }
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
                                variant.source_rect = SDL_Rect{0, 0,
                                                                (v < cache.widths.size()) ? cache.widths[v] : 0,
                                                                (v < cache.heights.size()) ? cache.heights[v] : 0};
                                variant.uses_atlas = (v < cache.uses_atlas.size()) ? cache.uses_atlas[v] : false;
                                f.variants.push_back(variant);
                            }
                        }

                        if (i < anchor_frames.size()) {
                                f.set_anchor_points(anchor_frames[i]);
                        } else {
                                f.set_anchor_points({});
                        }

                        if (f.dx != 0 || f.dy != 0 || f.dz != 0) {
                                any_motion = true;
                        }

                if (path_idx == 0) {
                        animation.frames.push_back(&f);
                }
        }
}

        apply_frame_boxes(animation.movement_paths_, hit_box_frames, attack_box_frames);

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
				float fw = 0.0f;
				float fh = 0.0f;
				if (SDL_GetTextureSize(animation.frame_cache_[0].textures[0], &fw, &fh)) {
					frame_width = static_cast<int>(std::lround(fw));
					frame_height = static_cast<int>(std::lround(fh));
				}
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

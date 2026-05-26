#include "animation_loader.hpp"
#include "animation.hpp"
#include "animation_cloner.hpp"
#include "anchor_point.hpp"
#include "asset_info.hpp"
#include "asset_types.hpp"
#include "animation/controllers/shared/attack_payload.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "json_coercion.hpp"
#include "surface_utils.hpp"
#include "utils/cache_manager.hpp"
#include "utils/loading_status_notifier.hpp"
#include "utils/log.hpp"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace {

using json_coercion::read_bool_field_like;
using json_coercion::read_float_field_like;
using json_coercion::read_int_field_like;
using json_coercion::read_string_field_like;

std::string read_on_end_value(const nlohmann::json& payload) {
        if (!payload.is_object() || !payload.contains("on_end")) {
                return "default";
        }
        const nlohmann::json& value = payload["on_end"];
        if (value.is_null()) {
                return "default";
        }
        if (!value.is_string()) {
                return "default";
        }
        std::string on_end = value.get<std::string>();
        if (on_end.empty()) {
                return "default";
        }
        std::string lowered = on_end;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
        });
        if (lowered == "default" || lowered == "loop" || lowered == "kill" || lowered == "lock" ||
            lowered == "reverse") {
                return lowered;
        }
        return on_end;
}

bool box_trace_enabled() {
        static const bool enabled = [] {
                const char* raw = SDL_getenv("VIBBLE_BOX_TRACE");
                if (!raw || !*raw) {
                        return false;
                }
                const std::string value(raw);
                return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
        }();
        return enabled;
}

std::string source_animation_id(const Animation& animation) {
        if (animation.source.kind != "animation") {
                return {};
        }
        if (!animation.source.name.empty()) {
                return animation.source.name;
        }
        return animation.source.path;
}

bool animation_inherits_data(const Animation& animation) {
        return animation.inherit_data && !source_animation_id(animation).empty();
}

std::string normalize_tag_value(std::string_view raw) {
        const auto begin = std::find_if_not(raw.begin(), raw.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
        });
        const auto end = std::find_if_not(raw.rbegin(), raw.rend(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
        }).base();
        if (begin >= end) {
                return {};
        }

        std::string normalized(begin, end);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
        });
        return normalized;
}

std::vector<std::string> parse_animation_tags(const nlohmann::json& payload) {
        std::vector<std::string> tags;
        std::unordered_set<std::string> seen;
        if (!payload.is_object() || !payload.contains("tags")) {
                return tags;
        }

        auto append_tag = [&](const nlohmann::json& node) {
                if (!node.is_string()) {
                        return;
                }
                std::string normalized = normalize_tag_value(node.get<std::string>());
                if (normalized.empty()) {
                        return;
                }
                if (seen.insert(normalized).second) {
                        tags.push_back(std::move(normalized));
                }
        };

        const nlohmann::json& tag_node = payload["tags"];
        if (tag_node.is_array()) {
                for (const auto& entry : tag_node) {
                        append_tag(entry);
                }
        } else {
                append_tag(tag_node);
        }
        return tags;
}

void apply_movement_transforms(std::vector<std::vector<AnimationFrame>>& paths,
                               bool                                      reverse_frames,
                               bool                                      invert_x,
                               bool                                      invert_y,
                               bool                                      invert_z) {
        if (reverse_frames) {
                for (auto& path : paths) {
                        std::reverse(path.begin(), path.end());
                }
        }
        if (invert_x) {
                for (auto& path : paths) {
                        for (auto& frame : path) {
                                frame.dx = -frame.dx;
                        }
                }
        }
        if (invert_y) {
                for (auto& path : paths) {
                        for (auto& frame : path) {
                                frame.dy = -frame.dy;
                        }
                }
        }
        if (invert_z) {
                for (auto& path : paths) {
                        for (auto& frame : path) {
                                frame.dz = -frame.dz;
                        }
                }
        }
}

float invert_horizontal_rotation(float rotation_degrees) {
        if (!std::isfinite(rotation_degrees)) {
                return 0.0f;
        }
        return -rotation_degrees;
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

DisplacedAssetAnchorPoint read_anchor_point(const nlohmann::json& node,
                                            bool& valid,
                                            bool default_flip_horizontal,
                                            bool default_flip_vertical,
                                            float default_rotation_degrees,
                                            bool default_hidden,
                                            bool default_resolve_x,
                                            AnchorScalingMethod default_scaling_method) {
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
        if (!node.contains("depth_offset") || !node["depth_offset"].is_number()) {
                return anchor;
        }

        anchor.texture_x = node["texture_x"].get<int>();
        anchor.texture_y = node["texture_y"].get<int>();
        anchor.depth_offset = read_float_field_like(node, "depth_offset", 0.0f);
        if (!std::isfinite(anchor.depth_offset)) {
                anchor.depth_offset = 0.0f;
        }
        anchor.flip_horizontal = read_bool_field_like(node, "flip_horizontal", default_flip_horizontal);
        anchor.flip_vertical = read_bool_field_like(node, "flip_vertical", default_flip_vertical);
        anchor.rotation_degrees = read_float_field_like(node, "rotation_degrees", default_rotation_degrees);
        anchor.hidden = read_bool_field_like(node, "hidden", default_hidden);
        anchor.resolve_x = read_bool_field_like(node, "resolve_x", default_resolve_x);
        anchor.scaling_method = anchor_points::anchor_scaling_method_from_token(
            read_string_field_like(
                node,
                "scaling_method",
                std::string(anchor_points::anchor_scaling_method_to_token(default_scaling_method))),
            default_scaling_method);
        if (const auto light_it = node.find("light");
            light_it != node.end() && light_it->is_object()) {
                const nlohmann::json& light = *light_it;
                anchor.has_light_data = true;
                anchor.light.enabled = read_bool_field_like(light, "enabled", false);
                anchor.light.opacity = read_float_field_like(light, "opacity", anchor.light.opacity);
                anchor.light.intensity = read_float_field_like(light, "intensity", anchor.light.intensity);
                anchor.light.radius = read_float_field_like(light, "radius", anchor.light.radius);
                anchor.light.falloff = read_float_field_like(light, "falloff", anchor.light.falloff);
                anchor.light.shadow_strength =
                    read_float_field_like(light, "shadow_strength", anchor.light.shadow_strength);
                anchor.light.cast_shadows = read_bool_field_like(light, "cast_shadows", anchor.light.cast_shadows);

                if (light.contains("color")) {
                        const nlohmann::json& color_node = light["color"];
                        int r = static_cast<int>(anchor.light.color_r);
                        int g = static_cast<int>(anchor.light.color_g);
                        int b = static_cast<int>(anchor.light.color_b);
                        if (color_node.is_array() && color_node.size() >= 3) {
                                if (color_node[0].is_number()) r = static_cast<int>(color_node[0].get<double>());
                                if (color_node[1].is_number()) g = static_cast<int>(color_node[1].get<double>());
                                if (color_node[2].is_number()) b = static_cast<int>(color_node[2].get<double>());
                        } else if (color_node.is_object()) {
                                r = read_int_field_like(color_node, "r", r);
                                g = read_int_field_like(color_node, "g", g);
                                b = read_int_field_like(color_node, "b", b);
                        }
                        anchor.light.color_r = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                        anchor.light.color_g = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                        anchor.light.color_b = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
                }
                anchor.light.sanitize();
        }
        if (!std::isfinite(anchor.rotation_degrees)) {
                anchor.rotation_degrees = default_rotation_degrees;
        }

        valid = anchor.is_valid();
        return anchor;
}

std::vector<std::vector<DisplacedAssetAnchorPoint>> parse_anchor_frames(const nlohmann::json& anchor_json,
                                                                        std::size_t           frame_count,
                                                                        bool default_flip_horizontal,
                                                                        bool default_flip_vertical,
                                                                        float default_rotation_degrees,
                                                                        bool default_hidden,
                                                                        bool default_resolve_x,
                                                                        AnchorScalingMethod default_scaling_method) {
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
                        auto anchor = read_anchor_point(node,
                                                        ok,
                                                        default_flip_horizontal,
                                                        default_flip_vertical,
                                                        default_rotation_degrees,
                                                        default_hidden,
                                                        default_resolve_x,
                                                        default_scaling_method);
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
                             bool reverse_frames,
                             bool invert_x,
                             bool invert_y,
                             bool invert_z) {
        if (anchors.empty()) {
                return;
        }
        if (reverse_frames) {
                std::reverse(anchors.begin(), anchors.end());
        }
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
                        if (invert_x) {
                                if (frame_w > 0) {
                                        anchor.texture_x = frame_w - 1 - anchor.texture_x;
                                }
                                anchor.flip_horizontal = !anchor.flip_horizontal;
                                anchor.rotation_degrees = invert_horizontal_rotation(anchor.rotation_degrees);
                        }
                        if (invert_y) {
                                if (frame_h > 0) {
                                        anchor.texture_y = frame_h - 1 - anchor.texture_y;
                                }
                                anchor.flip_vertical = !anchor.flip_vertical;
                        }
                        if (invert_z) {
                                anchor.depth_offset = -anchor.depth_offset;
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

std::string sanitize_box_id(std::string value) {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
                const unsigned char uch = static_cast<unsigned char>(ch);
                if (std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.') {
                        out.push_back(static_cast<char>(std::tolower(uch)));
                } else if (std::isspace(uch) != 0) {
                        out.push_back('_');
                }
        }
        return out;
}

std::string make_default_box_id(const char* kind_prefix,
                                std::size_t frame_index,
                                std::size_t ordinal) {
        return sanitize_box_id(std::string(kind_prefix) +
                               "_" +
                               std::to_string(frame_index + 1) +
                               "_" +
                               std::to_string(ordinal + 1));
}

std::string make_unique_id(const std::string& desired,
                           const char* fallback_prefix,
                           std::size_t frame_index,
                           std::size_t ordinal,
                           std::unordered_set<std::string>& used_ids) {
        std::string base = sanitize_box_id(desired);
        if (base.empty()) {
                base = make_default_box_id(fallback_prefix, frame_index, ordinal);
        }
        std::string candidate = base;
        int suffix = 2;
        while (!used_ids.insert(candidate).second) {
                candidate = base + "_" + std::to_string(suffix++);
        }
        return candidate;
}

int read_box_int_from_path(const nlohmann::json& node,
                           const char* object_key,
                           const char* nested_key,
                           int fallback) {
        if (!node.is_object() || !node.contains(object_key) || !node[object_key].is_object()) {
                return fallback;
        }
        return read_int_field_like(node[object_key], nested_key, fallback);
}

template <typename TBox>
std::string box_id_list(const std::vector<TBox>& boxes) {
        std::ostringstream out;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
                if (i > 0) {
                        out << ",";
                }
                out << boxes[i].id;
        }
        return out.str();
}

animation_update::FrameBoxRect parse_box_rect(const nlohmann::json& node) {
        const bool has_position = node.is_object() && node.contains("position") && node["position"].is_object();
        const bool has_size = node.is_object() && node.contains("size") && node["size"].is_object();
        if (has_position && has_size) {
                const int x = read_box_int_from_path(node, "position", "x", 0);
                const int y = read_box_int_from_path(node, "position", "y", 0);
                const int w = std::max(0, read_box_int_from_path(node, "size", "w", 0));
                const int h = std::max(0, read_box_int_from_path(node, "size", "h", 0));
                return animation_update::FrameBoxRect{x, y, x + w, y + h}.normalized_clamped();
        }

        std::vector<animation_update::FrameBoxCorner> parsed_corners;
        if (!node.is_object()) {
                return animation_update::FrameBoxRect{};
        }
        const nlohmann::json& corners = (node.contains("corners") && node["corners"].is_array())
                                            ? node["corners"]
                                            : nlohmann::json::array();
        parsed_corners.reserve(corners.size());
        for (std::size_t idx = 0; idx < corners.size(); ++idx) {
                if (!corners[idx].is_object()) {
                        continue;
                }
                const auto& corner_node = corners[idx];
                parsed_corners.push_back(animation_update::FrameBoxCorner{
                    std::max(0, read_int_field_like(corner_node, "texture_x", 0)),
                    std::max(0, read_int_field_like(corner_node, "texture_y", 0)),
                });
        }
        if (!parsed_corners.empty()) {
                return animation_update::FrameBoxRect::from_points(parsed_corners);
        }
        return animation_update::FrameBoxRect{};
}

template <typename TBox>
void parse_box_common_fields(TBox& box,
                             const nlohmann::json& node,
                             const char* fallback_name_prefix,
                             const char* default_type,
                             std::size_t frame_index,
                             std::size_t ordinal) {
        if (!node.is_object()) {
                return;
        }

        box.id = sanitize_box_id(read_string_field_like(
            node,
            "id",
            make_default_box_id(default_type, frame_index, ordinal)));
        box.type = read_string_field_like(node, "type", std::string{default_type});
        box.name = read_string_field_like(node,
                                          "name",
                                          std::string{fallback_name_prefix + std::string{"_"} + std::to_string(ordinal + 1)});
        box.enabled = read_bool_field_like(node, "enabled", true);
        box.extrusion_forward = std::max(1, read_int_field_like(node, "extrusion_forward", 1));
        box.extrusion_backward = std::max(1, read_int_field_like(node, "extrusion_backward", 1));
        box.anchor_link = read_string_field_like(node, "anchor_link", std::string{});
        box.set_rotation_degrees(read_float_field_like(node, "rotation_degrees", 0.0f));
        box.frame_start = static_cast<int>(frame_index);
        box.frame_end = static_cast<int>(frame_index);
        if (node.contains("frame_range") && node["frame_range"].is_object()) {
                const auto& range = node["frame_range"];
                box.frame_start = read_int_field_like(range, "start", box.frame_start);
                box.frame_end = read_int_field_like(range, "end", box.frame_end);
                if (box.frame_end < box.frame_start) {
                        box.frame_end = box.frame_start;
                }
        }
        box.set_rect(parse_box_rect(node));
}

animation_update::FrameHitBox parse_hit_box(const nlohmann::json& node,
                                            std::size_t frame_index,
                                            std::size_t ordinal) {
        animation_update::FrameHitBox box{};
        if (!node.is_object()) {
                return box;
        }
        parse_box_common_fields(box, node, "hit_box", "hitbox", frame_index, ordinal);
        return box;
}

animation_update::FrameAttackBox parse_attack_box(const nlohmann::json& node,
                                                  std::size_t frame_index,
                                                  std::size_t ordinal) {
        animation_update::FrameAttackBox box{};
        if (!node.is_object()) {
                return box;
        }
        parse_box_common_fields(box, node, "attack_box", "attack_box", frame_index, ordinal);
        const int fallback_damage = read_int_field_like(node, "damage_amount", 0);
        const std::string fallback_payload_id = read_string_field_like(node, "payload_id", box.id);
        if (node.contains("meta") && node["meta"].is_object()) {
                box.meta_json = node["meta"].dump();
        } else {
                box.meta_json = "{}";
        }
        box.payload = animation_update::attack_payload_from_box(fallback_damage, fallback_payload_id, box.meta_json);
        box.damage_amount = box.payload.damage_amount;
        box.payload_id = box.payload.payload_id.empty() ? box.id : box.payload.payload_id;
        box.payload.payload_id = box.payload_id;
        box.meta_json = animation_update::merge_attack_payload_into_meta_json(box.meta_json, box.payload);
        return box;
}

std::vector<animation_update::FrameHitBox> parse_hit_box_frame(const nlohmann::json& entry,
                                                               std::size_t frame_index) {
        std::vector<animation_update::FrameHitBox> boxes;
        if (!entry.is_array()) {
                return boxes;
        }
        std::unordered_set<std::string> used_names;
        std::unordered_set<std::string> used_ids;
        boxes.reserve(entry.size());
        for (std::size_t idx = 0; idx < entry.size(); ++idx) {
                if (!entry[idx].is_object()) {
                        continue;
                }
                animation_update::FrameHitBox box = parse_hit_box(entry[idx], frame_index, idx);
                box.name = make_unique_name(box.name, "hit_box", used_names, idx);
                box.id = make_unique_id(box.id, "hitbox", frame_index, idx, used_ids);
                box.type = "hitbox";
                if (box.is_valid()) {
                        boxes.push_back(std::move(box));
                }
        }
        return boxes;
}

std::vector<animation_update::FrameAttackBox> parse_attack_box_frame(const nlohmann::json& entry,
                                                                     std::size_t frame_index) {
        std::vector<animation_update::FrameAttackBox> boxes;
        if (!entry.is_array()) {
                return boxes;
        }
        std::unordered_set<std::string> used_names;
        std::unordered_set<std::string> used_ids;
        boxes.reserve(entry.size());
        for (std::size_t idx = 0; idx < entry.size(); ++idx) {
                if (!entry[idx].is_object()) {
                        continue;
                }
                animation_update::FrameAttackBox box = parse_attack_box(entry[idx], frame_index, idx);
                box.name = make_unique_name(box.name, "attack_box", used_names, idx);
                box.id = make_unique_id(box.id, "attack_box", frame_index, idx, used_ids);
                box.type = "attack_box";
                if (box.payload_id.empty()) {
                        box.payload_id = box.id;
                }
                box.payload.payload_id = box.payload_id;
                box.payload.damage_amount = std::max(0, box.payload.damage_amount);
                box.damage_amount = box.payload.damage_amount;
                box.meta_json = animation_update::merge_attack_payload_into_meta_json(box.meta_json, box.payload);
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
                result[frame_idx] = parse_hit_box_frame(hit_boxes_json[frame_idx], frame_idx);
                if (box_trace_enabled()) {
                        SDL_Log("[BoxFlow][deserialize] kind=hitbox frame=%zu count=%zu ids=%s",
                                frame_idx,
                                result[frame_idx].size(),
                                box_id_list(result[frame_idx]).c_str());
                }
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
                result[frame_idx] = parse_attack_box_frame(attack_boxes_json[frame_idx], frame_idx);
                if (box_trace_enabled()) {
                        SDL_Log("[BoxFlow][deserialize] kind=attack_box frame=%zu count=%zu ids=%s",
                                frame_idx,
                                result[frame_idx].size(),
                                box_id_list(result[frame_idx]).c_str());
                }
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
                          bool invert_x,
                          bool invert_y) {
        if (boxes.empty()) {
                return;
        }
        if (reverse_frames) {
                std::reverse(boxes.begin(), boxes.end());
        }
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
                        animation_update::FrameBoxRect flipped = box.rect;
                        if (invert_x && frame_w > 0) {
                                const int next_left = frame_w - 1 - box.rect.right;
                                const int next_right = frame_w - 1 - box.rect.left;
                                flipped.left = next_left;
                                flipped.right = next_right;
                        }
                        if (invert_y && frame_h > 0) {
                                const int next_top = frame_h - 1 - box.rect.bottom;
                                const int next_bottom = frame_h - 1 - box.rect.top;
                                flipped.top = next_top;
                                flipped.bottom = next_bottom;
                        }
                        box.set_rect(flipped);
                        if (invert_x) {
                                box.set_rotation_degrees(-box.rotation_degrees);
                        }
                        if (invert_y) {
                                box.set_rotation_degrees(-box.rotation_degrees);
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
                        if (!hit_boxes.empty() && idx < hit_boxes.size()) {
                                frame.set_hit_boxes(hit_boxes[idx]);
                        }
                        if (!attack_boxes.empty() && idx < attack_boxes.size()) {
                                frame.set_attack_boxes(attack_boxes[idx]);
                        }
                }
        }
}

bool bind_frame_data(Animation&                                                       animation,
                     const std::vector<std::vector<DisplacedAssetAnchorPoint>>&       anchor_frames,
                     const std::vector<std::vector<animation_update::FrameHitBox>>&   hit_box_frames,
                     const std::vector<std::vector<animation_update::FrameAttackBox>>& attack_box_frames) {
        const std::size_t frame_count = animation.cached_frame_count();
        std::vector<std::vector<AnimationFrame>> paths;
        for (std::size_t path_idx = 0; path_idx < animation.movement_path_count(); ++path_idx) {
                paths.push_back(animation.movement_path(path_idx));
        }
        if (paths.empty()) {
                paths.emplace_back();
        }

        std::vector<std::vector<DisplacedAssetAnchorPoint>> anchors = anchor_frames;
        std::vector<std::vector<animation_update::FrameHitBox>> hit_boxes = hit_box_frames;
        std::vector<std::vector<animation_update::FrameAttackBox>> attack_boxes = attack_box_frames;
        if (anchors.size() < frame_count) anchors.resize(frame_count);
        if (!hit_boxes.empty() && hit_boxes.size() < frame_count) hit_boxes.resize(frame_count);
        if (!attack_boxes.empty() && attack_boxes.size() < frame_count) attack_boxes.resize(frame_count);

        bool any_motion = false;
        for (std::size_t path_idx = 0; path_idx < paths.size(); ++path_idx) {
                auto& path = paths[path_idx];
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
                        f.set_anchor_points(i < anchors.size() ? anchors[i] : std::vector<DisplacedAssetAnchorPoint>{});

                        if (f.dx != 0 || f.dy != 0 || f.dz != 0) {
                                any_motion = true;
                        }
                }
        }

        apply_frame_boxes(paths, hit_boxes, attack_boxes);
        animation.replace_movement_paths(std::move(paths));
        animation.synchronize_runtime_frames();

        animation.total_dx = 0;
        animation.total_dy = 0;
        animation.total_dz = 0;
        animation.total_dr = 0.0f;
        if (animation.movement_path_count() > 0) {
                const auto& primary = animation.movement_path(0);
                for (const auto& frame : primary) {
                        animation.total_dx += frame.dx;
                        animation.total_dy += frame.dy;
                        animation.total_dz += frame.dz;
                        animation.total_dr += frame.rotation_degrees;
                        if (frame.dx != 0 || frame.dy != 0 || frame.dz != 0) {
                                any_motion = true;
                        }
                }
        }

        animation.movment = any_motion;
        return any_motion;
}

void resolve_inherited_frame_data(AssetInfo& info) {
        for (auto& [animation_id, animation] : info.animations) {
                if (!animation_inherits_data(animation)) {
                        continue;
                }

                const std::string source_id = source_animation_id(animation);
                auto source_it = info.animations.find(source_id);
                if (source_it == info.animations.end()) {
                        continue;
                }

                const Animation& source = source_it->second;
                const std::size_t frame_count = animation.cached_frame_count();

                std::vector<std::vector<AnimationFrame>> inherited_paths;
                for (std::size_t path_idx = 0; path_idx < source.movement_path_count(); ++path_idx) {
                        inherited_paths.push_back(source.movement_path(path_idx));
                }
                if (inherited_paths.empty()) {
                        inherited_paths.emplace_back();
                }
                apply_movement_transforms(inherited_paths,
                                          animation.reverse_source,
                                          animation.invert_x,
                                          animation.invert_y,
                                          animation.invert_z);
                animation.replace_movement_paths(std::move(inherited_paths));

                auto anchor_frames = collect_anchor_frames_from_animation(source, frame_count);
                apply_anchor_transforms(anchor_frames,
                                        animation.cached_frames(),
                                        animation.reverse_source,
                                        animation.invert_x,
                                        animation.invert_y,
                                        animation.invert_z);

                auto hit_box_frames = collect_hit_box_frames_from_animation(source, frame_count);
                apply_box_transforms(hit_box_frames,
                                     animation.cached_frames(),
                                     animation.reverse_source,
                                     animation.invert_x,
                                     animation.invert_y);

                auto attack_box_frames = collect_attack_box_frames_from_animation(source, frame_count);
                apply_box_transforms(attack_box_frames,
                                     animation.cached_frames(),
                                     animation.reverse_source,
                                     animation.invert_x,
                                     animation.invert_y);

                bind_frame_data(animation, anchor_frames, hit_box_frames, attack_box_frames);
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

void enforce_canonical_variant_layout(std::vector<float>& steps,
                                      std::vector<Animation::FrameCache>* frame_cache = nullptr) {
        render_pipeline::ScalingLogic::NormalizeVariantSteps(steps);
        const std::size_t canonical_variant_count = steps.size();
        if (!frame_cache) {
                return;
        }
        for (auto& cache : *frame_cache) {
                cache.textures.resize(canonical_variant_count, nullptr);
                cache.widths.resize(canonical_variant_count, 0);
                cache.heights.resize(canonical_variant_count, 0);
                cache.source_rects.resize(canonical_variant_count, SDL_Rect{0, 0, 0, 0});
                cache.uses_atlas.resize(canonical_variant_count, false);
            }
}

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
        animation.variant_steps_.clear();
        animation.tags.clear();
        enforce_canonical_variant_layout(animation.variant_steps_);
        (void)root_cache;

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
        animation.tags = parse_animation_tags(anim_json);
        // Optional lighting channels are asset-level metadata; runtime falls back to flat normal + diffuse when unavailable.
        (void)info.lighting_normal_map;
        info.lighting_roughness = std::clamp(info.lighting_roughness, 0.0f, 1.0f);
        if (!std::isfinite(info.lighting_height_bias)) info.lighting_height_bias = 0.0f;

        if (animation.source.kind == "animation" && !animation.source.name.empty()) {
                auto it = info.animations.find(animation.source.name);
                if (it != info.animations.end()) {
                        source_animation_ptr = &it->second;
                }
        }

        animation.invert_x = read_bool_field_like(anim_json, "invert_x", false);
        animation.invert_y = read_bool_field_like(anim_json, "invert_y", false);
        animation.invert_z = read_bool_field_like(anim_json, "invert_z", false);
        animation.reverse_source = read_bool_field_like(anim_json, "reverse_source", false);
        animation.invert_frames_horizontal = read_bool_field_like(anim_json, "invert_frames_horizontal", false);
        animation.invert_frames_vertical = read_bool_field_like(anim_json, "invert_frames_vertical", false);
        if (animation.source.kind == "animation" && anim_json.contains("derived_modifiers") &&
            anim_json["derived_modifiers"].is_object()) {
                const auto& modifiers = anim_json["derived_modifiers"];
                animation.reverse_source = read_bool_field_like(modifiers, "reverse", animation.reverse_source);
        }

        animation.locked = read_bool_field_like(anim_json, "locked", false);
        animation.randomize = read_bool_field_like(anim_json, "randomize", false);
        animation.rnd_start = read_bool_field_like(anim_json, "rnd_start", false);
        animation.on_end_animation = read_on_end_value(anim_json);
        animation.on_end_behavior = Animation::classify_on_end(animation.on_end_animation);
        animation.total_dx = 0;
        animation.total_dy = 0;
        animation.total_dz = 0;
        animation.total_dr = 0.0f;
        animation.movement_paths_.clear();
        animation.audio_clip = Animation::AudioClip{};
        const bool movement_enabled = info.is_movement_enabled();
        const bool hitbox_enabled = info.is_hitbox_enabled();
        const bool attack_box_enabled = info.is_attack_box_enabled();
        nlohmann::json anchor_points_json = nlohmann::json::array();
        const bool has_anchor_points_json = anim_json.contains("anchor_points") && anim_json["anchor_points"].is_array();
        if (has_anchor_points_json) {
                anchor_points_json = anim_json["anchor_points"];
        }
        nlohmann::json hit_boxes_json = nlohmann::json::array();
        const bool has_hit_boxes_json =
            hitbox_enabled && anim_json.contains("hit_boxes") && anim_json["hit_boxes"].is_array();
        if (has_hit_boxes_json) {
                hit_boxes_json = anim_json["hit_boxes"];
        }
        nlohmann::json attack_boxes_json = nlohmann::json::array();
        const bool has_attack_boxes_json =
            attack_box_enabled && anim_json.contains("attack_boxes") && anim_json["attack_boxes"].is_array();
        if (has_attack_boxes_json) {
                attack_boxes_json = anim_json["attack_boxes"];
        }
        const bool has_movement_json =
            movement_enabled && anim_json.contains("movement") && anim_json["movement"].is_array();
        const bool has_movement_paths_json =
            movement_enabled && anim_json.contains("movement_paths") && anim_json["movement_paths"].is_array();
        const bool has_any_local_frame_data =
            has_movement_json || has_movement_paths_json || has_anchor_points_json || has_hit_boxes_json || has_attack_boxes_json;
        const bool default_inherit_data =
            (animation.source.kind == "animation") && !has_any_local_frame_data;
        const bool inherit_data = read_bool_field_like(anim_json, "inherit_data", default_inherit_data);
        animation.inherit_data = (animation.source.kind == "animation") && inherit_data;
        const bool allow_geometry_inversion = (animation.source.kind == "animation") && animation.inherit_data;
        if (!allow_geometry_inversion) {
                animation.invert_x = false;
                animation.invert_y = false;
                animation.invert_z = false;
        }

        auto parse_movement_sequence = [&](const nlohmann::json& seq, std::vector<AnimationFrame>& dest) {
                bool specified = false;
                if (!seq.is_array()) return specified;
                auto clamp = [](int v) { return (v < 0) ? 0 : (v > 255 ? 255 : v); };
                auto read_int_like = [](const nlohmann::json& node, const char* key, int fallback) -> int {
                        if (!node.is_object() || !node.contains(key)) {
                                return fallback;
                        }
                        return read_int_field_like(node, key, fallback);
                };
                auto is_legacy_object_depth_in_y = [&](const nlohmann::json& node, int parsed_height, int parsed_depth) -> bool {
                        if (!node.is_object()) {
                                return false;
                        }
                        const bool has_height_key = node.contains("dy") || node.contains("y");
                        const bool has_depth_key = node.contains("dz") || node.contains("z");
                        if (!has_height_key) {
                                return false;
                        }
                        if (!has_depth_key) {
                                return true;
                        }

                        const bool has_xy_object_keys = node.contains("x") || node.contains("y") || node.contains("z");
                        const bool has_dxyz_keys = node.contains("dx") || node.contains("dy") || node.contains("dz");
                        if (has_xy_object_keys && has_dxyz_keys && parsed_depth == 0 && parsed_height != 0) {
                                return true;
                        }
                        return false;
                };

                for (const auto& mv : seq) {
                        AnimationFrame fm;

                        if (mv.is_object()) {
                                // Canonical axes: X/Z are floor movement, Y is height.
                                const int dx = mv.contains("dx") ? read_int_like(mv, "dx", 0) : read_int_like(mv, "x", 0);
                                int height_y = mv.contains("dy") ? read_int_like(mv, "dy", 0) : read_int_like(mv, "y", 0);
                                int depth_z = mv.contains("dz") ? read_int_like(mv, "dz", 0) : read_int_like(mv, "z", 0);

                                if (is_legacy_object_depth_in_y(mv, height_y, depth_z)) {
                                        depth_z = height_y;
                                        height_y = 0;
                                }

                                fm.dx = dx;
                                fm.dy = height_y;
                                fm.dz = depth_z;
                                fm.rotation_degrees = read_float_field_like(mv, "rotation_degrees", 0.0f);
                                fm.z_resort = read_bool_field_like(mv, "resort_z", false);
                                if (fm.dx != 0 || fm.dy != 0 || fm.dz != 0 ||
                                    std::abs(fm.rotation_degrees) > 1e-5f ||
                                    mv.contains("resort_z")) {
                                        specified = true;
                                }
                                dest.push_back(std::move(fm));
                                continue;
                        }

                        if (mv.is_array()) {
                                bool has_numeric_depth = false;
                                bool has_numeric_height = false;
                                int parsed_depth = 0;
                                int parsed_height = 0;
                                if (!mv.empty() && mv[0].is_number()) {
                                        try { fm.dx = mv[0].get<int>(); } catch (...) { fm.dx = static_cast<int>(std::lround(mv[0].get<double>())); }
                                }
                                if (mv.size() > 1 && mv[1].is_number()) {
                                        has_numeric_height = true;
                                        try { parsed_height = mv[1].get<int>(); } catch (...) { parsed_height = static_cast<int>(std::lround(mv[1].get<double>())); }
                                }
                                if (mv.size() > 2 && mv[2].is_number()) {
                                        has_numeric_depth = true;
                                        try { parsed_depth = mv[2].get<int>(); } catch (...) { parsed_depth = static_cast<int>(std::lround(mv[2].get<double>())); }
                                }

                                // Legacy arrays used [dx, depth]. Canonical arrays are [dx, y(height), z(depth)].
                                if (has_numeric_depth) {
                                        fm.dy = parsed_height;
                                        fm.dz = parsed_depth;
                                } else if (has_numeric_height) {
                                        fm.dy = 0;
                                        fm.dz = parsed_height;
                                }

                                if (mv.size() > 2 && mv[2].is_boolean()) {
                                        fm.z_resort = mv[2].get<bool>();
                                }
                                if (mv.size() > 3 && mv[3].is_number()) {
                                        try {
                                                fm.rotation_degrees = static_cast<float>(mv[3].get<double>());
                                        } catch (...) {
                                                fm.rotation_degrees = 0.0f;
                                        }
                                }
                                for (std::size_t bool_index = 3; bool_index < mv.size(); ++bool_index) {
                                        if (mv[bool_index].is_boolean()) {
                                                fm.z_resort = mv[bool_index].get<bool>();
                                                break;
                                        }
                                }
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

                        if (fm.dx != 0 || fm.dy != 0 || fm.dz != 0 ||
                            std::abs(fm.rotation_degrees) > 1e-5f ||
                            mv.size() >= 3) {
                                specified = true;
                        }
                        dest.push_back(std::move(fm));
                }
                return specified;
};

        std::vector<std::vector<AnimationFrame>> parsed_paths;
        if (has_movement_paths_json) {
                for (const auto& path_json : anim_json["movement_paths"]) {
                        std::vector<AnimationFrame> path_frames;
                        parse_movement_sequence(path_json, path_frames);
                        if (!path_frames.empty()) {
                                parsed_paths.push_back(std::move(path_frames));
                        } else {
                                parsed_paths.emplace_back();
                        }
                }
        }

        std::vector<AnimationFrame> primary_path;
        if (!has_movement_paths_json && has_movement_json) {
                parse_movement_sequence(anim_json["movement"], primary_path);
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
                        if (src_anim.has_frames()) {
                                AnimationCloner::Options opts{};
                                opts.flip_horizontal = animation.invert_frames_horizontal;
                                opts.flip_vertical   = animation.invert_frames_vertical;
                                opts.reverse_frames  = animation.reverse_source;
                                opts.invert_movement_x = animation.inherit_data && animation.invert_x;
                                opts.invert_movement_y = animation.inherit_data && animation.invert_y;
                                opts.invert_movement_z = animation.inherit_data && animation.invert_z;
                                opts.inherit_on_end_from_source = animation.inherit_data;

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
                        enforce_canonical_variant_layout(animation.variant_steps_, &animation.frame_cache_);
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

        enforce_canonical_variant_layout(animation.variant_steps_, &animation.frame_cache_);

        if (animation.frame_cache_.empty() &&
            animation.source.kind == "animation" &&
            !animation.source.name.empty()) {
                auto src_it = info.animations.find(animation.source.name);
                if (src_it != info.animations.end() && !src_it->second.frame_cache_.empty()) {
                        AnimationCloner::Options opts{};
                        opts.flip_horizontal = animation.invert_frames_horizontal;
                        opts.flip_vertical   = animation.invert_frames_vertical;
                        opts.reverse_frames  = animation.reverse_source;
                        opts.invert_movement_x = animation.inherit_data && animation.invert_x;
                        opts.invert_movement_y = animation.inherit_data && animation.invert_y;
                        opts.invert_movement_z = animation.inherit_data && animation.invert_z;
                        opts.inherit_on_end_from_source = animation.inherit_data;
                        std::cout << "[AnimationLoader] " << info.name << "::" << trigger
                                  << " late-cloning from source animation '" << animation.source.name
                                  << "' (flipH=" << opts.flip_horizontal
                                  << ", flipV=" << opts.flip_vertical
                                  << ", reverse=" << opts.reverse_frames << ")\n";
                        if (AnimationCloner::Clone(src_it->second, animation, opts, renderer, info)) {
                                reused_animation = true;
                        }
                }
        }

        const bool use_inherited_data = animation_inherits_data(animation);
        if (movement_enabled && use_inherited_data) {
                auto it = info.animations.find(source_animation_id(animation));
                if (it != info.animations.end()) {
                        // Post-migration contract: authored movement is authoritative.
                        (void)it;
                }
        }
        if (movement_enabled && !(use_inherited_data && reused_animation)) {
                animation.movement_paths_ = authored_movement_paths;
        } else if (!movement_enabled) {
                animation.movement_paths_.assign(1, {});
        }
        // Post-migration contract: movement comes directly from authored payload only.
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
                const bool is_file_sourced_animation = (animation.source.kind != "animation");
                anchor_frames = parse_anchor_frames(anchor_points_json,
                                                    frame_count,
                                                    is_file_sourced_animation,
                                                    is_file_sourced_animation,
                                                    0.0f,
                                                    false,
                                                    true,
                                                    AnchorScalingMethod::Parent);
        } else if (source_animation_ptr && use_inherited_data) {
                anchor_frames = collect_anchor_frames_from_animation(*source_animation_ptr, frame_count);
                apply_anchor_transforms(anchor_frames,
                                        animation.frame_cache_,
                                        animation.reverse_source,
                                        animation.invert_x,
                                        animation.invert_y,
                                        animation.invert_z);
        }
        std::vector<std::vector<animation_update::FrameHitBox>> hit_box_frames;
        if (hitbox_enabled && has_hit_boxes_json) {
                hit_box_frames = parse_hit_box_frames(hit_boxes_json, frame_count);
        } else if (hitbox_enabled && source_animation_ptr && use_inherited_data) {
                hit_box_frames = collect_hit_box_frames_from_animation(*source_animation_ptr, frame_count);
                apply_box_transforms(hit_box_frames,
                                     animation.frame_cache_,
                                     animation.reverse_source,
                                     animation.invert_x,
                                     animation.invert_y);
        }

        std::vector<std::vector<animation_update::FrameAttackBox>> attack_box_frames;
        if (attack_box_enabled && has_attack_boxes_json) {
                attack_box_frames = parse_attack_box_frames(attack_boxes_json, frame_count);
        } else if (attack_box_enabled && source_animation_ptr && use_inherited_data) {
                attack_box_frames = collect_attack_box_frames_from_animation(*source_animation_ptr, frame_count);
                apply_box_transforms(attack_box_frames,
                                     animation.frame_cache_,
                                     animation.reverse_source,
                                     animation.invert_x,
                                     animation.invert_y);
        }
        bind_frame_data(animation, anchor_frames, hit_box_frames, attack_box_frames);
        if (trigger == "default") {
                if (const AnimationFrame* first = animation.primary_frame_at(0); first && !first->variants.empty()) {
                        base_sprite = first->variants[0].base_texture;
                        info.preview_texture = first->variants[0].base_texture;
                }
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

        resolve_inherited_frame_data(info);
        flush_diagnostics();
}

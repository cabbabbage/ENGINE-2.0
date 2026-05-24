#include "PlaybackSettingsPanel.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <utility>

#include "AnimationDocument.hpp"
#include "json_coercion.hpp"

#include <nlohmann/json.hpp>

#include "PanelLayoutConstants.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "string_utils.hpp"

namespace {

constexpr int kItemGap = 8;
constexpr int kInversionPreviewHeight = 66;

using animation_editor::kPanelPadding;
using animation_editor::strings::trim_copy;
using json_coercion::read_bool_field_like;
using json_coercion::read_string_field_like;

namespace fs = std::filesystem;

int message_block_height(const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return 0;
    }
    const DMLabelStyle& style = DMStyles::Label();
    const int line_height = style.font_size + DMSpacing::small_gap();
    return static_cast<int>(lines.size()) * line_height;
}

void render_message_lines(SDL_Renderer* renderer, const SDL_Rect& rect, const std::vector<std::string>& lines) {
    if (!renderer || lines.empty()) {
        return;
    }
    const DMLabelStyle& style = DMStyles::Label();
    const int line_height = style.font_size + DMSpacing::small_gap();
    int y = rect.y;
    for (const auto& line : lines) {
        DMFontCache::instance().draw_text(renderer, style, line, rect.x, y);
        y += line_height;
    }
}

void render_inversion_preview(SDL_Renderer* renderer, const SDL_Rect& rect, bool invert_x, bool invert_y, bool invert_z) {
    if (!renderer || rect.w <= 0 || rect.h <= 0) {
        return;
    }

    const SDL_Color border = DMStyles::BorderColor();
    const SDL_Color text = DMStyles::TextMuted();
    const SDL_Color x_color{237, 85, 85, 255};
    const SDL_Color y_color{95, 205, 120, 255};
    const SDL_Color z_color{109, 177, 255, 255};

    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
    SDL_RenderRect(renderer, &rect);

    const int cx = rect.x + rect.w / 2;
    const int cy = rect.y + rect.h / 2 + 6;
    const int len = std::max(10, std::min(rect.w, rect.h) / 3);

    auto draw_axis = [&](int dx, int dy, const SDL_Color& color, const char* label) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        SDL_RenderLine(renderer, static_cast<float>(cx), static_cast<float>(cy),
                       static_cast<float>(cx + dx), static_cast<float>(cy + dy));
        DMFontCache::instance().draw_text(renderer, DMStyles::Label(), label, cx + dx + 3, cy + dy - 6);
    };

    draw_axis(invert_x ? -len : len, 0, x_color, "X");
    draw_axis(0, invert_y ? len : -len, y_color, "Y");
    draw_axis(invert_z ? -len : len, invert_z ? len : -len, z_color, "Z");

    DMFontCache::instance().draw_text(renderer, DMStyles::Label(), "Invert preview", rect.x + 6, rect.y + 4);
    std::string state_text = std::string("X:") + (invert_x ? "<" : ">") +
                             "  Y:" + (invert_y ? "v" : "^") +
                             "  Z:" + (invert_z ? "\\\\" : "//");
    DMFontCache::instance().draw_text(renderer, DMStyles::Label(), state_text, rect.x + 6, rect.y + rect.h - 18);
    SDL_SetRenderDrawColor(renderer, text.r, text.g, text.b, 255);
}

const std::vector<std::string>& invert_frames_help_lines() {
    static const std::vector<std::string> lines = {
        "Invert Frames applies to cloned frames even when Source Data is not inherited."
    };
    return lines;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool payload_uses_animation_source(const nlohmann::json& payload) {
    return payload.contains("source") &&
           payload["source"].is_object() &&
           payload["source"].value("kind", std::string{}) == "animation";
}

std::string payload_source_animation_id(const nlohmann::json& payload) {
    if (!payload_uses_animation_source(payload)) {
        return {};
    }
    const auto& source = payload["source"];
    std::string reference = trim_copy(source.value("name", std::string{}));
    if (!reference.empty()) {
        return reference;
    }
    return trim_copy(source.value("path", std::string{}));
}

struct SourceChainClassification {
    bool derived_from_animation = false;
    bool frame_sourced_direct_or_upstream = false;
    std::string direct_source_id;
};

SourceChainClassification classify_source_chain(const animation_editor::AnimationDocument* document,
                                                const std::string& animation_id,
                                                const nlohmann::json& payload) {
    (void)document;
    (void)animation_id;
    SourceChainClassification classification;
    if (!payload.is_object()) {
        return classification;
    }

    if (!payload_uses_animation_source(payload)) {
        classification.frame_sourced_direct_or_upstream = true;
        return classification;
    }

    classification.derived_from_animation = true;
    classification.direct_source_id = payload_source_animation_id(payload);
    return classification;
}

bool payload_has_local_frame_data(const nlohmann::json& payload) {
    return (payload.contains("movement") && payload["movement"].is_array()) ||
           (payload.contains("anchor_points") && payload["anchor_points"].is_array()) ||
           (payload.contains("hit_boxes") && payload["hit_boxes"].is_array()) ||
           (payload.contains("attack_boxes") && payload["attack_boxes"].is_array());
}

bool payload_inherits_data(const nlohmann::json& payload) {
    if (!payload_uses_animation_source(payload)) {
        return false;
    }
    const bool default_inherit = !payload_has_local_frame_data(payload);
    return read_bool_field_like(payload, "inherit_data", default_inherit);
}

std::string payload_on_end_value(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return "default";
    }
    std::string on_end = trim_copy(read_string_field_like(payload, "on_end", std::string{"default"}));
    if (on_end.empty()) {
        on_end = "default";
    }
    return on_end;
}

std::string source_on_end_value(const animation_editor::AnimationDocument* document,
                                const nlohmann::json& payload) {
    if (!document) {
        return "default";
    }
    const std::string source_id = payload_source_animation_id(payload);
    if (source_id.empty()) {
        return "default";
    }
    const auto source_payload = document->animation_payload_json(source_id);
    if (!source_payload.has_value() || !source_payload->is_object()) {
        return "default";
    }
    return payload_on_end_value(*source_payload);
}

std::size_t payload_frame_count(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return 1;
    }
    if (payload.contains("number_of_frames") && payload["number_of_frames"].is_number_integer()) {
        return static_cast<std::size_t>(std::max(payload["number_of_frames"].get<int>(), 1));
    }
    if (payload.contains("number_of_frames") && payload["number_of_frames"].is_number()) {
        return static_cast<std::size_t>(std::max(static_cast<int>(payload["number_of_frames"].get<double>()), 1));
    }
    return 1;
}

nlohmann::json normalize_frame_array_key(const nlohmann::json& payload,
                                         const char* key,
                                         std::size_t frame_count) {
    nlohmann::json value = (payload.contains(key) && payload[key].is_array())
                               ? payload[key]
                               : nlohmann::json::array();
    if (value.size() < frame_count) {
        value.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array());
    } else if (value.size() > frame_count) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(frame_count), value.end());
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_array()) {
            value[i] = nlohmann::json::array();
        }
    }
    return value;
}

int read_movement_component(const nlohmann::json& entry, int index) {
    auto read_number = [](const nlohmann::json& value) -> int {
        if (value.is_number_integer()) {
            return value.get<int>();
        }
        if (value.is_number()) {
            return static_cast<int>(std::lround(value.get<double>()));
        }
        return 0;
    };

    if (entry.is_array()) {
        if (index == 2 && entry.size() == 2 && entry[1].is_number()) {
            return read_number(entry[1]);
        }
        if (index == 1 && entry.size() == 2 && entry[1].is_number()) {
            return 0;
        }
        if (index < static_cast<int>(entry.size()) && entry[index].is_number()) {
            return read_number(entry[index]);
        }
        return 0;
    }

    if (!entry.is_object()) {
        return 0;
    }

    if (index == 0) {
        if (entry.contains("dx")) return read_number(entry["dx"]);
        if (entry.contains("x")) return read_number(entry["x"]);
        return 0;
    }

    if (index == 1) {
        const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
        const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
        const int legacy_height = entry.contains("dy")
            ? read_number(entry["dy"])
            : (entry.contains("y") ? read_number(entry["y"]) : 0);
        const int explicit_depth = entry.contains("dz")
            ? read_number(entry["dz"])
            : (entry.contains("z") ? read_number(entry["z"]) : 0);
        if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_height != 0) {
            return 0;
        }
        if (entry.contains("dy")) return read_number(entry["dy"]);
        if (entry.contains("y")) return read_number(entry["y"]);
        return 0;
    }

    const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
    const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
    const int legacy_depth = entry.contains("dy")
        ? read_number(entry["dy"])
        : (entry.contains("y") ? read_number(entry["y"]) : 0);
    const int explicit_depth = entry.contains("dz")
        ? read_number(entry["dz"])
        : (entry.contains("z") ? read_number(entry["z"]) : 0);
    if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_depth != 0) {
        return legacy_depth;
    }
    if (entry.contains("dz")) return read_number(entry["dz"]);
    if (entry.contains("z")) return read_number(entry["z"]);
    if (!entry.contains("dz") && !entry.contains("z")) {
        if (entry.contains("dy")) return read_number(entry["dy"]);
        if (entry.contains("y")) return read_number(entry["y"]);
    }
    return 0;
}

nlohmann::json canonical_movement_entry(const nlohmann::json& entry) {
    nlohmann::json result = nlohmann::json::array();
    result.push_back(read_movement_component(entry, 0));
    result.push_back(read_movement_component(entry, 1));
    result.push_back(read_movement_component(entry, 2));

    bool resort_z = false;
    bool has_resort = false;
    nlohmann::json preserved_color;
    bool has_color = false;

    if (entry.is_array()) {
        for (const auto& node : entry) {
            if (node.is_boolean()) {
                resort_z = node.get<bool>();
                has_resort = true;
            }
            const bool looks_color = node.is_array() &&
                                     node.size() == 3 &&
                                     node[0].is_number() &&
                                     node[1].is_number() &&
                                     node[2].is_number();
            if (looks_color && !has_color) {
                preserved_color = node;
                has_color = true;
            }
        }
    } else if (entry.is_object() && entry.contains("resort_z") && entry["resort_z"].is_boolean()) {
        resort_z = entry["resort_z"].get<bool>();
        has_resort = true;
    }

    if (has_resort) {
        result.push_back(resort_z);
    }
    if (has_color) {
        result.push_back(std::move(preserved_color));
    }
    return result;
}

std::vector<nlohmann::json> canonical_movement_frames(const nlohmann::json& payload, std::size_t frame_count) {
    std::vector<nlohmann::json> movement(frame_count, nlohmann::json::array({0, 0, 0}));
    if (!payload.contains("movement") || !payload["movement"].is_array()) {
        return movement;
    }
    const auto& raw_movement = payload["movement"];
    const std::size_t limit = std::min(frame_count, raw_movement.size());
    for (std::size_t i = 0; i < limit; ++i) {
        movement[i] = canonical_movement_entry(raw_movement[i]);
    }
    return movement;
}

void set_movement_components(nlohmann::json& entry, int dx, int dy, int dz) {
    nlohmann::json updated = entry.is_array() ? entry : nlohmann::json::array();
    if (updated.size() < 3) {
        updated = nlohmann::json::array();
        updated.push_back(dx);
        updated.push_back(dy);
        updated.push_back(dz);
    } else {
        updated[0] = dx;
        updated[1] = dy;
        updated[2] = dz;
    }
    entry = std::move(updated);
}

void apply_movement_transforms(std::vector<nlohmann::json>& movement,
                               bool reverse_frames,
                               bool invert_x,
                               bool invert_y,
                               bool invert_z) {
    if (reverse_frames) {
        std::reverse(movement.begin(), movement.end());
    }
    if (!invert_x && !invert_y && !invert_z) {
        return;
    }
    for (auto& entry : movement) {
        const int dx = read_movement_component(entry, 0);
        const int dy = read_movement_component(entry, 1);
        const int dz = read_movement_component(entry, 2);
        set_movement_components(entry,
                                invert_x ? -dx : dx,
                                invert_y ? -dy : dy,
                                invert_z ? -dz : dz);
    }
}

template <typename T>
void resize_with_last(std::vector<T>& items, std::size_t frame_count, const T& fallback) {
    if (items.empty()) {
        items.resize(frame_count, fallback);
        return;
    }
    if (items.size() > frame_count) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(frame_count), items.end());
        return;
    }
    const T tail = items.back();
    items.resize(frame_count, tail);
}

SDL_Point read_image_size(const fs::path& path) {
    if (path.empty()) {
        return SDL_Point{0, 0};
    }
    SDL_Surface* surface = IMG_Load(path.string().c_str());
    if (!surface) {
        return SDL_Point{0, 0};
    }
    SDL_Point size{surface->w, surface->h};
    SDL_DestroySurface(surface);
    return size;
}

std::vector<fs::path> find_frame_sequence(const fs::path& asset_root,
                                          const nlohmann::json& payload,
                                          const std::string& animation_id) {
    std::vector<fs::path> numeric_frames;
    std::vector<fs::path> fallback_sequence;
    bool has_fallback_sequence = false;
    std::error_code ec;

    const int requested_frames = static_cast<int>(payload_frame_count(payload));
    std::string relative_path = animation_id;
    if (payload.contains("source") && payload["source"].is_object()) {
        relative_path = payload["source"].value("path", relative_path);
    }
    if (relative_path.empty()) {
        relative_path = animation_id;
    }

    fs::path folder = asset_root;
    fs::path requested = relative_path;
    const auto should_treat_as_absolute = [&](const fs::path& path) {
        if (path.is_absolute()) {
            return true;
        }
        const std::string requested_str = lowercase_copy(path.generic_string());
        if (requested_str.rfind("src/", 0) == 0) {
            return true;
        }
        if (!asset_root.empty()) {
            const std::string root_str = lowercase_copy(asset_root.generic_string());
            if (!root_str.empty()) {
                if (requested_str == root_str) {
                    return true;
                }
                if (requested_str.rfind(root_str + "/", 0) == 0) {
                    return true;
                }
            }
        }
        return false;
    };

    if (should_treat_as_absolute(requested)) {
        folder = requested;
    } else if (!requested.empty()) {
        folder = asset_root.empty() ? requested : (asset_root / requested);
    }

    if (requested_frames > 0) {
        fallback_sequence.reserve(static_cast<std::size_t>(requested_frames));
        fs::path fallback;
        for (int i = 0; i < requested_frames; ++i) {
            fs::path candidate = folder / (std::to_string(i) + ".png");
            if (fs::exists(candidate, ec)) {
                fallback_sequence.push_back(candidate);
                if (fallback.empty()) {
                    fallback = candidate;
                }
            } else {
                fallback_sequence.emplace_back();
            }
            ec.clear();
        }
        if (!fallback.empty()) {
            for (auto& path : fallback_sequence) {
                if (path.empty()) {
                    path = fallback;
                }
            }
            has_fallback_sequence = true;
        } else {
            fallback_sequence.clear();
        }
    }

    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
        return has_fallback_sequence ? fallback_sequence : std::vector<fs::path>{};
    }

    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path& path = entry.path();
        if (lowercase_copy(path.extension().string()) != ".png") {
            continue;
        }
        try {
            (void)std::stoi(path.stem().string());
            numeric_frames.push_back(path);
        } catch (...) {
        }
    }

    if (numeric_frames.empty()) {
        return has_fallback_sequence ? fallback_sequence : std::vector<fs::path>{};
    }

    std::sort(numeric_frames.begin(), numeric_frames.end(), [](const fs::path& lhs, const fs::path& rhs) {
        int left = 0;
        int right = 0;
        try {
            left = std::stoi(lhs.stem().string());
        } catch (...) {
        }
        try {
            right = std::stoi(rhs.stem().string());
        } catch (...) {
        }
        return left < right;
    });

    if (requested_frames > 0) {
        const int available = static_cast<int>(numeric_frames.size());
        const int target = std::max(requested_frames, available);
        std::vector<fs::path> sequence;
        sequence.reserve(static_cast<std::size_t>(target));
        for (int i = 0; i < target; ++i) {
            sequence.push_back(i < available ? numeric_frames[static_cast<std::size_t>(i)] : numeric_frames.back());
        }
        return sequence;
    }

    return numeric_frames;
}

std::vector<SDL_Point> resolve_display_frame_sizes(const animation_editor::AnimationDocument* document,
                                                   const std::string& animation_id,
                                                   std::unordered_set<std::string>& visited) {
    if (!document || animation_id.empty() || !visited.insert(animation_id).second) {
        return {};
    }

    const auto payload_opt = document->animation_payload_json(animation_id);
    if (!payload_opt.has_value() || !payload_opt->is_object()) {
        return {SDL_Point{0, 0}};
    }

    const nlohmann::json& payload = *payload_opt;
    const std::size_t frame_count = payload_frame_count(payload);

    std::vector<SDL_Point> sizes;
    if (payload_uses_animation_source(payload)) {
        const std::string source_id = payload_source_animation_id(payload);
        sizes = resolve_display_frame_sizes(document, source_id, visited);
        resize_with_last(sizes, frame_count, SDL_Point{0, 0});
        const bool reverse = payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()
            ? read_bool_field_like(payload["derived_modifiers"], "reverse", read_bool_field_like(payload, "reverse_source", false))
            : read_bool_field_like(payload, "reverse_source", false);
        if (reverse) {
            std::reverse(sizes.begin(), sizes.end());
        }
        return sizes;
    }

    const std::vector<fs::path> frames = find_frame_sequence(document->asset_root(), payload, animation_id);
    sizes.reserve(frames.size());
    for (const auto& frame : frames) {
        sizes.push_back(read_image_size(frame));
    }
    resize_with_last(sizes, frame_count, SDL_Point{0, 0});
    if (read_bool_field_like(payload, "reverse_source", false)) {
        std::reverse(sizes.begin(), sizes.end());
    }
    return sizes;
}

void apply_anchor_frame_flip(nlohmann::json& frame,
                             const SDL_Point& size,
                             bool invert_x,
                             bool invert_y,
                             bool invert_z) {
    if (!frame.is_array()) {
        frame = nlohmann::json::array();
        return;
    }
    for (auto& anchor : frame) {
        if (!anchor.is_object()) {
            continue;
        }
        if (invert_x && size.x > 0 && anchor.contains("texture_x") && anchor["texture_x"].is_number()) {
            const int x = anchor["texture_x"].is_number_integer()
                ? anchor["texture_x"].get<int>()
                : static_cast<int>(std::lround(anchor["texture_x"].get<double>()));
            anchor["texture_x"] = size.x - 1 - x;
        }
        if (invert_y && size.y > 0 && anchor.contains("texture_y") && anchor["texture_y"].is_number()) {
            const int y = anchor["texture_y"].is_number_integer()
                ? anchor["texture_y"].get<int>()
                : static_cast<int>(std::lround(anchor["texture_y"].get<double>()));
            anchor["texture_y"] = size.y - 1 - y;
        }
        if (invert_z && anchor.contains("depth_offset") && anchor["depth_offset"].is_number()) {
            const double depth = anchor["depth_offset"].is_number_integer()
                ? static_cast<double>(anchor["depth_offset"].get<int>())
                : anchor["depth_offset"].get<double>();
            anchor["depth_offset"] = std::isfinite(depth) ? -depth : 0.0;
        }
    }
}

void apply_box_frame_flip(nlohmann::json& frame, const SDL_Point& size, bool flip_x, bool flip_y) {
    if (!frame.is_array()) {
        frame = nlohmann::json::array();
        return;
    }
    for (auto& box : frame) {
        if (!box.is_object()) {
            continue;
        }

        auto read_int_like_field = [](const nlohmann::json& node, const char* key, int fallback) {
            if (!node.is_object() || !node.contains(key) || !node[key].is_number()) {
                return fallback;
            }
            return node[key].is_number_integer()
                ? node[key].get<int>()
                : static_cast<int>(std::lround(node[key].get<double>()));
        };

        bool has_rect = false;
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;

        const bool has_position = box.contains("position") && box["position"].is_object();
        const bool has_size_obj = box.contains("size") && box["size"].is_object();
        if (has_position && has_size_obj) {
            const int x = read_int_like_field(box["position"], "x", 0);
            const int y = read_int_like_field(box["position"], "y", 0);
            const int w = std::max(0, read_int_like_field(box["size"], "w", 0));
            const int h = std::max(0, read_int_like_field(box["size"], "h", 0));
            left = x;
            top = y;
            right = x + w;
            bottom = y + h;
            has_rect = true;
        } else if (box.contains("corners") && box["corners"].is_array()) {
            bool first_corner = true;
            for (const auto& corner : box["corners"]) {
                if (!corner.is_object()) {
                    continue;
                }
                const int x = read_int_like_field(corner, "texture_x", 0);
                const int y = read_int_like_field(corner, "texture_y", 0);
                if (first_corner) {
                    left = right = x;
                    top = bottom = y;
                    first_corner = false;
                } else {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
            has_rect = !first_corner;
        }

        if (!has_rect) {
            continue;
        }

        if (flip_x && size.x > 0) {
            const int next_left = size.x - 1 - right;
            const int next_right = size.x - 1 - left;
            left = next_left;
            right = next_right;
        }
        if (flip_y && size.y > 0) {
            const int next_top = size.y - 1 - bottom;
            const int next_bottom = size.y - 1 - top;
            top = next_top;
            bottom = next_bottom;
        }
        if (right < left) {
            std::swap(left, right);
        }
        if (bottom < top) {
            std::swap(top, bottom);
        }

        box["position"] = nlohmann::json::object({
            {"x", left},
            {"y", top},
        });
        box["size"] = nlohmann::json::object({
            {"w", std::max(0, right - left)},
            {"h", std::max(0, bottom - top)},
        });
        box["corners"] = nlohmann::json::array({
            nlohmann::json{{"texture_x", left}, {"texture_y", top}},
            nlohmann::json{{"texture_x", right}, {"texture_y", top}},
            nlohmann::json{{"texture_x", right}, {"texture_y", bottom}},
            nlohmann::json{{"texture_x", left}, {"texture_y", bottom}},
        });
    }
}
struct GeometryResolution {
    std::vector<nlohmann::json> movement;
    std::vector<nlohmann::json> anchors;
    std::vector<nlohmann::json> hit_boxes;
    std::vector<nlohmann::json> attack_boxes;
    std::vector<SDL_Point> frame_sizes;
    bool valid = false;
};

GeometryResolution resolve_geometry(const animation_editor::AnimationDocument* document,
                                    const std::string& animation_id,
                                    std::unordered_set<std::string>& visited) {
    GeometryResolution result;
    if (!document || animation_id.empty() || !visited.insert(animation_id).second) {
        return result;
    }

    const auto payload_opt = document->animation_payload_json(animation_id);
    if (!payload_opt.has_value() || !payload_opt->is_object()) {
        return result;
    }

    const nlohmann::json& payload = *payload_opt;
    const std::size_t frame_count = payload_frame_count(payload);
    std::unordered_set<std::string> size_visited;
    result.frame_sizes = resolve_display_frame_sizes(document, animation_id, size_visited);
    resize_with_last(result.frame_sizes, frame_count, SDL_Point{0, 0});

    if (payload_uses_animation_source(payload) && payload_inherits_data(payload)) {
        const std::string source_id = payload_source_animation_id(payload);
        GeometryResolution inherited = resolve_geometry(document, source_id, visited);
        if (!inherited.valid) {
            return result;
        }

        result.movement = inherited.movement;
        result.anchors = inherited.anchors;
        result.hit_boxes = inherited.hit_boxes;
        result.attack_boxes = inherited.attack_boxes;
        resize_with_last(result.movement, frame_count, nlohmann::json::array({0, 0, 0}));
        resize_with_last(result.anchors, frame_count, nlohmann::json::array());
        resize_with_last(result.hit_boxes, frame_count, nlohmann::json::array());
        resize_with_last(result.attack_boxes, frame_count, nlohmann::json::array());

        bool reverse = read_bool_field_like(payload, "reverse_source", false);
        bool invert_x = read_bool_field_like(payload, "invert_x", false);
        bool invert_y = read_bool_field_like(payload, "invert_y", false);
        bool invert_z = read_bool_field_like(payload, "invert_z", false);
        if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
            const auto& modifiers = payload["derived_modifiers"];
            reverse = read_bool_field_like(modifiers, "reverse", reverse);
        }

        if (reverse) {
            std::reverse(result.movement.begin(), result.movement.end());
            std::reverse(result.anchors.begin(), result.anchors.end());
            std::reverse(result.hit_boxes.begin(), result.hit_boxes.end());
            std::reverse(result.attack_boxes.begin(), result.attack_boxes.end());
            std::reverse(result.frame_sizes.begin(), result.frame_sizes.end());
        }
        apply_movement_transforms(result.movement, false, invert_x, invert_y, invert_z);
        for (std::size_t i = 0; i < frame_count; ++i) {
            const SDL_Point size = i < result.frame_sizes.size() ? result.frame_sizes[i] : SDL_Point{0, 0};
            apply_anchor_frame_flip(result.anchors[i], size, invert_x, invert_y, invert_z);
            apply_box_frame_flip(result.hit_boxes[i], size, invert_x, invert_y);
            apply_box_frame_flip(result.attack_boxes[i], size, invert_x, invert_y);
        }

        result.valid = true;
        return result;
    }

    result.movement = canonical_movement_frames(payload, frame_count);
    result.anchors.reserve(frame_count);
    result.hit_boxes.reserve(frame_count);
    result.attack_boxes.reserve(frame_count);

    const nlohmann::json anchors = normalize_frame_array_key(payload, "anchor_points", frame_count);
    const nlohmann::json hit_boxes = normalize_frame_array_key(payload, "hit_boxes", frame_count);
    const nlohmann::json attack_boxes = normalize_frame_array_key(payload, "attack_boxes", frame_count);
    for (std::size_t i = 0; i < frame_count; ++i) {
        result.anchors.push_back(anchors[i]);
        result.hit_boxes.push_back(hit_boxes[i]);
        result.attack_boxes.push_back(attack_boxes[i]);
    }

    result.valid = true;
    return result;
}

void update_payload_movement_total(nlohmann::json& payload) {
    const auto movement = payload.contains("movement") && payload["movement"].is_array()
        ? payload["movement"]
        : nlohmann::json::array();
    int total_dx = 0;
    int total_dy = 0;
    int total_dz = 0;
    float total_dr = 0.0f;
    for (std::size_t i = 0; i < movement.size(); ++i) {
        total_dx += read_movement_component(movement[i], 0);
        total_dy += read_movement_component(movement[i], 1);
        total_dz += read_movement_component(movement[i], 2);
        if (movement[i].is_array() &&
            movement[i].size() > 3 &&
            movement[i][3].is_number()) {
            total_dr += static_cast<float>(movement[i][3].get<double>());
        } else if (movement[i].is_object() &&
                   movement[i].contains("rotation_degrees") &&
                   movement[i]["rotation_degrees"].is_number()) {
            total_dr += static_cast<float>(movement[i]["rotation_degrees"].get<double>());
        }
    }
    payload["movement_total"] =
        nlohmann::json{{"dx", total_dx}, {"dy", total_dy}, {"dz", total_dz}, {"dr", total_dr}};
}

void materialize_inherited_geometry(const animation_editor::AnimationDocument* document,
                                    const std::string& animation_id,
                                    nlohmann::json& payload) {
    if (!document || animation_id.empty() || !payload.is_object() || !payload_inherits_data(payload)) {
        return;
    }

    std::unordered_set<std::string> visited;
    GeometryResolution resolved = resolve_geometry(document, animation_id, visited);
    if (!resolved.valid) {
        return;
    }

    payload["movement"] = nlohmann::json::array();
    payload["anchor_points"] = nlohmann::json::array();
    payload["hit_boxes"] = nlohmann::json::array();
    payload["attack_boxes"] = nlohmann::json::array();
    for (const auto& frame : resolved.movement) {
        payload["movement"].push_back(frame);
    }
    for (const auto& frame : resolved.anchors) {
        payload["anchor_points"].push_back(frame);
    }
    for (const auto& frame : resolved.hit_boxes) {
        payload["hit_boxes"].push_back(frame);
    }
    for (const auto& frame : resolved.attack_boxes) {
        payload["attack_boxes"].push_back(frame);
    }
    payload.erase("hit_geometry");
    payload.erase("attack_geometry");
    update_payload_movement_total(payload);
}

}

namespace animation_editor {

PlaybackSettingsPanel::PlaybackSettingsPanel() {
    ensure_widgets();
}

void PlaybackSettingsPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    document_ = std::move(document);
    sync_from_document();
}

void PlaybackSettingsPanel::set_animation_id(const std::string& animation_id) {
    animation_id_ = animation_id;
    sync_from_document();
}

void PlaybackSettingsPanel::set_bounds(const SDL_Rect& bounds) {
    bounds_ = bounds;
    layout_dirty_ = true;
}

int PlaybackSettingsPanel::preferred_height(int width) const {
    const int padding = kPanelPadding;
    const int gap = kItemGap;
    const int checkbox_height = DMCheckbox::height();

    int height = padding;
    auto add_checkbox_group = [&](int count) {
        if (count <= 0) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            height += checkbox_height;
            if (i + 1 < count) {
                height += gap;
            }
        }
};

    if (derived_from_animation_) {
        int count = 1; // Reverse is always available for animation-sourced entries.
        count += 1; // Frame inversion checkbox
        if (inherit_controls_visible()) {
            ++count; // Inherit Source Data
            if (inversion_controls_visible()) {
                count += 3; // Invert Data X/Y/Z
            }
        }
        add_checkbox_group(count);
    } else {
        add_checkbox_group(2);
    }

    if (derived_from_animation_) {
        if (!inherited_message_lines_.empty()) {
            height += gap;
            height += message_block_height(inherited_message_lines_);
        }
        if (inversion_controls_visible()) {
            height += gap;
            height += kInversionPreviewHeight;
        }
    } else {
        if (random_start_visible()) {
            height += gap;
            height += checkbox_height;
        }
    }

    height += padding;
    return height;
}

void PlaybackSettingsPanel::update() {
    layout_widgets();
}

void PlaybackSettingsPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    layout_widgets();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect( renderer, bounds_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), DMStyles::PanelBG(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    auto render_checkbox = [&](const std::unique_ptr<DMCheckbox>& checkbox, bool visible) {
        if (visible && checkbox) {
            checkbox->render(renderer);
        }
};

    const bool show_frame_invert_controls = derived_from_animation_;
    const bool show_flip_controls = inversion_controls_visible();
    render_checkbox(invert_frames_horizontal_checkbox_, show_frame_invert_controls);
    render_checkbox(invert_frames_vertical_checkbox_, show_frame_invert_controls);
    render_checkbox(invert_x_checkbox_, show_flip_controls);
    render_checkbox(invert_y_checkbox_, show_flip_controls);
    render_checkbox(invert_z_checkbox_, show_flip_controls);
    if (show_flip_controls && inversion_preview_rect_.h > 0) {
        render_inversion_preview(renderer,
                                 inversion_preview_rect_,
                                 invert_x_checkbox_ && invert_x_checkbox_->value(),
                                 invert_y_checkbox_ && invert_y_checkbox_->value(),
                                 invert_z_checkbox_ && invert_z_checkbox_->value());
    }
    render_checkbox(inherit_geometry_checkbox_, inherit_controls_visible());

    render_checkbox(reverse_checkbox_, derived_from_animation_);
    render_checkbox(locked_checkbox_, !derived_from_animation_);
    if (!derived_from_animation_ && random_start_checkbox_ && (!locked_checkbox_ || !locked_checkbox_->value())) {
        random_start_checkbox_->render(renderer);
    }

    const auto& helper_lines = invert_frames_help_lines();
    if (invert_frames_helper_rect_.h > 0) {
        render_message_lines(renderer, invert_frames_helper_rect_, helper_lines);
    }

    DMWidgetTooltipRender(renderer, bounds_, info_tooltip_);
}

bool PlaybackSettingsPanel::handle_event(const SDL_Event& e) {
    layout_widgets();
    bool used = false;

    if (DMWidgetTooltipHandleEvent(e, bounds_, info_tooltip_)) {
        return true;
    }

    auto handle_checkbox = [&](std::unique_ptr<DMCheckbox>& checkbox) {
        if (checkbox && checkbox->handle_event(e)) {
            used = true;
            handle_controls_changed();
        }
};

    auto handle_checkbox_if_visible = [&](std::unique_ptr<DMCheckbox>& checkbox, bool visible) {
        if (visible) {
            handle_checkbox(checkbox);
        }
};

    const bool show_flip = inversion_controls_visible();
    handle_checkbox_if_visible(invert_frames_horizontal_checkbox_, derived_from_animation_);
    handle_checkbox_if_visible(invert_frames_vertical_checkbox_, derived_from_animation_);
    handle_checkbox_if_visible(invert_x_checkbox_, show_flip);
    handle_checkbox_if_visible(invert_y_checkbox_, show_flip);
    handle_checkbox_if_visible(invert_z_checkbox_, show_flip);
    handle_checkbox_if_visible(inherit_geometry_checkbox_, inherit_controls_visible());

    handle_checkbox_if_visible(reverse_checkbox_, derived_from_animation_);
    if (!derived_from_animation_) handle_checkbox(locked_checkbox_);
    if (!derived_from_animation_ && (!locked_checkbox_ || !locked_checkbox_->value())) {
        handle_checkbox(random_start_checkbox_);
    }

    return used;
}

void PlaybackSettingsPanel::ensure_widgets() {
    auto ensure_checkbox = [&](std::unique_ptr<DMCheckbox>& checkbox, const char* label) {
        if (!checkbox) {
            checkbox = std::make_unique<DMCheckbox>(label, false);
            layout_dirty_ = true;
        }
};

    ensure_checkbox(invert_frames_horizontal_checkbox_, "Invert Frames Horizontally");
    ensure_checkbox(invert_frames_vertical_checkbox_, "Invert Frames Vertically");
    ensure_checkbox(invert_x_checkbox_, "Invert Data X");
    ensure_checkbox(invert_y_checkbox_, "Invert Data Y");
    ensure_checkbox(invert_z_checkbox_, "Invert Data Z");
    ensure_checkbox(inherit_geometry_checkbox_, "Inherit Source Data");
    ensure_checkbox(reverse_checkbox_, "Play Frames In Reverse");
    ensure_checkbox(locked_checkbox_, "Locked (animation must finish before another can play)");
    ensure_checkbox(random_start_checkbox_, "Randomize Starting Frame");
}

void PlaybackSettingsPanel::layout_widgets() const {
    if (!layout_dirty_) {
        return;
    }

    const_cast<PlaybackSettingsPanel*>(this)->ensure_widgets();

    layout_dirty_ = false;

    if (bounds_.w <= 0 || bounds_.h <= 0) {
        return;
    }

    const int padding = kPanelPadding;
    const int gap = kItemGap;
    const int width = std::max(0, bounds_.w - padding * 2);
    int x = bounds_.x + padding;
    int y = bounds_.y + padding;

    auto place_checkbox = [&](DMCheckbox* checkbox, bool visible, bool& placed_any) {
        if (!checkbox) {
            return;
        }
        if (!visible) {
            checkbox->set_rect(SDL_Rect{0, 0, 0, 0});
            return;
        }
        if (placed_any) {
            y += gap;
        }
        SDL_Rect rect{x, y, width, DMCheckbox::height()};
        checkbox->set_rect(rect);
        y += rect.h;
        placed_any = true;
};

    bool placed_any_checkbox = false;
    const bool show_frame_invert_controls = derived_from_animation_;
    const bool show_flip_controls = inversion_controls_visible();
    place_checkbox(invert_frames_horizontal_checkbox_.get(), show_frame_invert_controls, placed_any_checkbox);
    place_checkbox(invert_frames_vertical_checkbox_.get(), show_frame_invert_controls, placed_any_checkbox);
    invert_frames_helper_rect_ = SDL_Rect{0, 0, 0, 0};
    if (show_frame_invert_controls) {
        const auto& helper_lines = invert_frames_help_lines();
        const int helper_height = message_block_height(helper_lines);
        if (helper_height > 0) {
            if (placed_any_checkbox) {
                y += gap;
            }
            invert_frames_helper_rect_ = SDL_Rect{x, y, width, helper_height};
            y += helper_height;
            placed_any_checkbox = true;
        }
    }
    place_checkbox(invert_x_checkbox_.get(), show_flip_controls, placed_any_checkbox);
    place_checkbox(invert_y_checkbox_.get(), show_flip_controls, placed_any_checkbox);
    place_checkbox(invert_z_checkbox_.get(), show_flip_controls, placed_any_checkbox);
    inversion_preview_rect_ = SDL_Rect{0, 0, 0, 0};
    if (show_flip_controls) {
        if (placed_any_checkbox) {
            y += gap;
        }
        inversion_preview_rect_ = SDL_Rect{x, y, width, kInversionPreviewHeight};
        y += kInversionPreviewHeight;
        placed_any_checkbox = true;
    }
    place_checkbox(inherit_geometry_checkbox_.get(), inherit_controls_visible(), placed_any_checkbox);

    place_checkbox(reverse_checkbox_.get(), derived_from_animation_, placed_any_checkbox);
    place_checkbox(locked_checkbox_.get(), !derived_from_animation_, placed_any_checkbox);

    if (derived_from_animation_) {
        if (random_start_checkbox_) {
            random_start_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        int message_height = message_block_height(inherited_message_lines_);
        if (message_height > 0) {
            if (placed_any_checkbox) {
                y += gap;
            }
            inherited_message_rect_ = SDL_Rect{x, y, width, message_height};
            y += message_height;
        } else {
            inherited_message_rect_ = SDL_Rect{0, 0, 0, 0};
        }
    } else {
        if (random_start_visible()) {
            place_checkbox(random_start_checkbox_.get(), true, placed_any_checkbox);
        } else if (random_start_checkbox_) {
            random_start_checkbox_->set_rect(SDL_Rect{0, 0, 0, 0});
        }
        inherited_message_rect_ = SDL_Rect{0, 0, 0, 0};
    }
}

void PlaybackSettingsPanel::apply_state_to_controls(const PlaybackState& state) {
    ensure_widgets();
    const bool show_inherit_controls = inherit_controls_visible_for_state(state);
    const bool show_inversion_controls = inversion_controls_visible_for_state(state);
    if (invert_frames_horizontal_checkbox_) {
        invert_frames_horizontal_checkbox_->set_value(state.invert_frames_horizontal);
    }
    if (invert_frames_vertical_checkbox_) {
        invert_frames_vertical_checkbox_->set_value(state.invert_frames_vertical);
    }
    if (invert_x_checkbox_) {
        invert_x_checkbox_->set_value(show_inversion_controls ? state.invert_x : false);
    }
    if (invert_y_checkbox_) {
        invert_y_checkbox_->set_value(show_inversion_controls ? state.invert_y : false);
    }
    if (invert_z_checkbox_) {
        invert_z_checkbox_->set_value(show_inversion_controls ? state.invert_z : false);
    }
    if (inherit_geometry_checkbox_) inherit_geometry_checkbox_->set_value(show_inherit_controls ? state.inherit_data : false);
    if (reverse_checkbox_) reverse_checkbox_->set_value(state.reverse_source);
    if (locked_checkbox_) locked_checkbox_->set_value(state.locked);
    if (random_start_checkbox_) {
        if (!random_start_visible_for_state(state) && random_start_checkbox_->value()) {
            random_start_checkbox_->set_value(false);
        }
        if (random_start_visible_for_state(state)) {
            random_start_checkbox_->set_value(state.random_start);
        } else {
            random_start_checkbox_->set_value(false);
        }
    }
}

PlaybackSettingsPanel::PlaybackState PlaybackSettingsPanel::read_controls() const {
    PlaybackState state = state_;
    if (derived_from_animation_) {
        if (invert_frames_horizontal_checkbox_) {
            state.invert_frames_horizontal = invert_frames_horizontal_checkbox_->value();
        }
        if (invert_frames_vertical_checkbox_) {
            state.invert_frames_vertical = invert_frames_vertical_checkbox_->value();
        }
        if (inherit_controls_visible_for_state(state)) {
            if (inherit_geometry_checkbox_) state.inherit_data = inherit_geometry_checkbox_->value();
        } else {
            state.inherit_data = false;
        }
        if (inversion_controls_visible_for_state(state)) {
            if (invert_x_checkbox_) state.invert_x = invert_x_checkbox_->value();
            if (invert_y_checkbox_) state.invert_y = invert_y_checkbox_->value();
            if (invert_z_checkbox_) state.invert_z = invert_z_checkbox_->value();
        } else {
            state.invert_x = false;
            state.invert_y = false;
            state.invert_z = false;
        }
    } else {
        state.invert_x = false;
        state.invert_y = false;
        state.invert_z = false;
        state.inherit_data = false;
        state.invert_frames_horizontal = false;
        state.invert_frames_vertical = false;
    }

    if (derived_from_animation_) {
        if (reverse_checkbox_) state.reverse_source = reverse_checkbox_->value();
    } else {
        state.reverse_source = false;
    }
    if (!derived_from_animation_) {
        if (locked_checkbox_) state.locked = locked_checkbox_->value();
    } else {
        state.locked = false;
    }
    if (!derived_from_animation_) {
        if (random_start_checkbox_ && (!locked_checkbox_ || !locked_checkbox_->value())) {
            state.random_start = random_start_checkbox_->value();
        } else {
            state.random_start = false;
        }
    } else {
        state.random_start = false;
    }

    if (state.locked) {
        state.random_start = false;
    }
    return state;
}

void PlaybackSettingsPanel::handle_controls_changed() {
    if (is_syncing_ui_) {
        return;
    }

    const bool previous_visibility = random_start_visible();
    const bool previous_inherit = state_.inherit_data;
    PlaybackState new_state = read_controls();
    const bool new_visibility = random_start_visible_for_state(new_state);

    if (!new_visibility && random_start_checkbox_ && random_start_checkbox_->value()) {
        is_syncing_ui_ = true;
        random_start_checkbox_->set_value(false);
        is_syncing_ui_ = false;
        new_state.random_start = false;
    }

    state_ = new_state;

    if (previous_visibility != new_visibility || previous_inherit != new_state.inherit_data) {
        layout_dirty_ = true;
    }

    if (!document_) {
        return;
    }

    if (has_document_state_ && new_state == document_state_) {
        return;
    }

    commit_changes(new_state);
}

void PlaybackSettingsPanel::sync_from_document() {
    ensure_widgets();

    PlaybackState new_state;
    bool found = false;
    nlohmann::json parsed_payload = nlohmann::json::object();

    if (document_ && !animation_id_.empty()) {
        if (auto payload = fetch_payload(document_.get(), animation_id_)) {
            parsed_payload = nlohmann::json::parse(*payload, nullptr, false);
            if (!parsed_payload.is_object()) {
                parsed_payload = nlohmann::json::object();
            }
            found = true;
        }
    }

    if (!found) {
        parsed_payload = nlohmann::json::object();
    }

    update_inherited_state(parsed_payload);
    if (found) {
        new_state = payload_to_state(parsed_payload);
    }

    state_ = new_state;
    document_state_ = new_state;
    has_document_state_ = found;

    is_syncing_ui_ = true;
    apply_state_to_controls(new_state);
    is_syncing_ui_ = false;

    layout_dirty_ = true;
}

void PlaybackSettingsPanel::commit_changes(const PlaybackState& desired_state) {
    if (!document_ || animation_id_.empty()) {
        return;
    }

    auto payload_dump = fetch_payload(document_.get(), animation_id_);
    if (!payload_dump) {
        return;
    }

    nlohmann::json payload = nlohmann::json::parse(*payload_dump, nullptr, false);
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }

    apply_state_to_payload(payload, desired_state);
    document_->update_animation_payload(animation_id_, payload);

    auto updated_dump = fetch_payload(document_.get(), animation_id_);
    if (!updated_dump) {
        return;
    }

    nlohmann::json updated = nlohmann::json::parse(*updated_dump, nullptr, false);
    if (!updated.is_object()) {
        updated = nlohmann::json::object();
    }

    update_inherited_state(updated);

    PlaybackState normalized = payload_to_state(updated);
    document_state_ = normalized;
    const bool previous_visibility = random_start_visible();
    state_ = normalized;
    has_document_state_ = true;

    is_syncing_ui_ = true;
    apply_state_to_controls(normalized);
    is_syncing_ui_ = false;

    if (previous_visibility != random_start_visible()) {
        layout_dirty_ = true;
    }
}

std::optional<std::string> PlaybackSettingsPanel::fetch_payload(const AnimationDocument* document,
                                                                const std::string& animation_id) {
    if (!document) {
        return std::nullopt;
    }
    return document->animation_payload(animation_id);
}

PlaybackSettingsPanel::PlaybackState PlaybackSettingsPanel::payload_to_state(const nlohmann::json& payload) {
    PlaybackState state;
    state.invert_x = read_bool_field_like(payload, "invert_x", false);
    state.invert_y = read_bool_field_like(payload, "invert_y", false);
    state.invert_z = read_bool_field_like(payload, "invert_z", false);
    state.reverse_source = read_bool_field_like(payload, "reverse_source", false);
    state.inherit_data = false;
    state.invert_frames_horizontal = false;
    state.invert_frames_vertical = false;
    state.locked         = read_bool_field_like(payload, "locked", false);
    state.random_start   = read_bool_field_like(payload, "rnd_start", false);
    if (state.locked) {
        state.random_start = false;
    }

    bool source_is_animation = false;
    if (payload.contains("source") && payload["source"].is_object()) {
        const nlohmann::json& source = payload["source"];
        std::string kind = source.value("kind", std::string{});
        if (kind == "animation") {
            source_is_animation = true;
            state.inherit_data = payload_inherits_data(payload);
            state.invert_frames_horizontal = read_bool_field_like(payload, "invert_frames_horizontal", false);
            state.invert_frames_vertical = read_bool_field_like(payload, "invert_frames_vertical", false);
            if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
                const auto& modifiers = payload["derived_modifiers"];
                state.reverse_source = read_bool_field_like(modifiers, "reverse", state.reverse_source);
            }
        }
    }

    if (!source_is_animation) {
        state.reverse_source = false;
        state.invert_x = false;
        state.invert_y = false;
        state.invert_z = false;
        state.invert_frames_horizontal = false;
        state.invert_frames_vertical = false;
    }

    if (!inherit_controls_visible_for_state(state)) {
        state.inherit_data = false;
    }

    if (!state.inherit_data) {
        state.invert_x = false;
        state.invert_y = false;
        state.invert_z = false;
    }

    return state;
}

void PlaybackSettingsPanel::apply_state_to_payload(nlohmann::json& payload, const PlaybackState& state) {
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }
    const bool allow_inherit_data = inherit_controls_visible_for_state(state);
    const bool effective_inherit_data = allow_inherit_data ? state.inherit_data : false;
    const PlaybackState previous_state = payload_to_state(payload);
    const bool toggled_to_inherit_data =
        derived_from_animation_ && !previous_state.inherit_data && effective_inherit_data;
    const bool toggled_to_local_data =
        derived_from_animation_ && previous_state.inherit_data && !effective_inherit_data;
    if (derived_from_animation_ && previous_state.inherit_data && !effective_inherit_data) {
        materialize_inherited_geometry(document_.get(), animation_id_, payload);
    }

    const bool allow_data_inversion = derived_from_animation_ && effective_inherit_data;
    const bool effective_invert_x = allow_data_inversion ? state.invert_x : false;
    const bool effective_invert_y = allow_data_inversion ? state.invert_y : false;
    const bool effective_invert_z = allow_data_inversion ? state.invert_z : false;

    payload["invert_x"] = effective_invert_x;
    payload["invert_y"] = effective_invert_y;
    payload["invert_z"] = effective_invert_z;
    payload["reverse_source"] = state.reverse_source;
    if (derived_from_animation_) {
        payload["invert_frames_horizontal"] = state.invert_frames_horizontal;
        payload["invert_frames_vertical"] = state.invert_frames_vertical;
    } else {
        payload.erase("invert_frames_horizontal");
        payload.erase("invert_frames_vertical");
    }
    if (!derived_from_animation_) {
        payload["locked"] = state.locked;
    } else {
        payload.erase("locked");
    }
    if (derived_from_animation_) {
        payload.erase("rnd_start");
        payload.erase("speed");
        payload.erase("speed_factor");
        payload.erase("speed_multiplier");
        payload.erase("fps");
        payload["inherit_data"] = effective_inherit_data;
        if (toggled_to_inherit_data) {
            payload["on_end"] = source_on_end_value(document_.get(), payload);
        } else if (toggled_to_local_data) {
            payload["on_end"] = "default";
        }
        nlohmann::json modifiers = nlohmann::json::object();
        modifiers["reverse"] = state.reverse_source;
        if (effective_inherit_data) {
            payload.erase("movement");
            payload.erase("movement_total");
            payload.erase("anchor_points");
            payload.erase("hit_boxes");
            payload.erase("attack_boxes");
            payload.erase("hit_geometry");
            payload.erase("attack_geometry");
        }
        payload["derived_modifiers"] = std::move(modifiers);
    } else {
        payload["rnd_start"]    = state.random_start && !state.locked;
        payload.erase("derived_modifiers");
        payload.erase("inherit_data");
        payload.erase("speed");
        payload.erase("fps");
        payload.erase("speed_factor");
        payload.erase("speed_multiplier");
    }
    payload.erase("inherit_source_geometry");
    payload.erase("flipped_source");
    payload.erase("flip_vertical_source");
    payload.erase("flip_movement_horizontal");
    payload.erase("flip_movement_vertical");
    if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
        payload["derived_modifiers"].erase("flipX");
        payload["derived_modifiers"].erase("flipY");
        payload["derived_modifiers"].erase("flipMovementX");
        payload["derived_modifiers"].erase("flipMovementY");
    }
}

void PlaybackSettingsPanel::update_inherited_state(const nlohmann::json& payload) {
    bool previous_flag = derived_from_animation_;
    bool previous_frame_chain = source_chain_resolves_to_frames_;
    std::string previous_source = derived_source_id_;

    const SourceChainClassification classification = classify_source_chain(document_.get(), animation_id_, payload);
    derived_from_animation_ = classification.derived_from_animation;
    source_chain_resolves_to_frames_ = classification.frame_sourced_direct_or_upstream;
    derived_source_id_ = classification.direct_source_id;
    inherited_modifiers_.clear();

    if (derived_from_animation_) {
        const bool inherit_source_data_enabled = !source_chain_resolves_to_frames_ && payload_inherits_data(payload);
        bool reverse = read_bool_field_like(payload, "reverse_source", false);
        const bool invert_x = inherit_source_data_enabled && read_bool_field_like(payload, "invert_x", false);
        const bool invert_y = inherit_source_data_enabled && read_bool_field_like(payload, "invert_y", false);
        const bool invert_z = inherit_source_data_enabled && read_bool_field_like(payload, "invert_z", false);
        if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
            const auto& modifiers = payload["derived_modifiers"];
            reverse = read_bool_field_like(modifiers, "reverse", reverse);
        }
        if (reverse) inherited_modifiers_.push_back("Reverse");
        if (invert_x) inherited_modifiers_.push_back("Invert X");
        if (invert_y) inherited_modifiers_.push_back("Invert Y");
        if (invert_z) inherited_modifiers_.push_back("Invert Z");
        if (inherit_source_data_enabled) {
            inherited_modifiers_.push_back("Inherit Source Data");
        } else {
            inherited_modifiers_.push_back("Local Source Data");
        }
    }

    refresh_inherited_message();

    if (previous_flag != derived_from_animation_ ||
        previous_frame_chain != source_chain_resolves_to_frames_ ||
        previous_source != derived_source_id_) {
        layout_dirty_ = true;
    }
}

bool PlaybackSettingsPanel::inherit_controls_visible_for_state(const PlaybackState& /*state*/) const {
    return derived_from_animation_ && !source_chain_resolves_to_frames_;
}

bool PlaybackSettingsPanel::inversion_controls_visible_for_state(const PlaybackState& state) const {
    return inherit_controls_visible_for_state(state) && state.inherit_data;
}

bool PlaybackSettingsPanel::random_start_visible_for_state(const PlaybackState& state) const {
    return !derived_from_animation_ && !state.locked;
}

void PlaybackSettingsPanel::refresh_inherited_message() {
    std::vector<std::string> previous_lines = inherited_message_lines_;
    inherited_message_lines_.clear();
    inherited_message_rect_ = SDL_Rect{0, 0, 0, 0};

    if (derived_from_animation_) {
        std::string target = derived_source_id_.empty() ? std::string("the source animation") : "animation '" + derived_source_id_ + "'";
        inherited_message_lines_.push_back("Lock and starting frame inherit from " + target + ".");
        if (!inherited_modifiers_.empty()) {
            std::string joined;
            for (size_t i = 0; i < inherited_modifiers_.size(); ++i) {
                if (i > 0) joined.append(", ");
                joined.append(inherited_modifiers_[i]);
            }
            inherited_message_lines_.push_back("Applied modifiers: " + joined + ".");
        }
        if (source_chain_resolves_to_frames_) {
            inherited_message_lines_.push_back("Inherit Source Data is unavailable because the source chain resolves to frames.");
        } else {
            inherited_message_lines_.push_back("Source data can inherit from the source or be materialized locally.");
        }
    }

    if (inherited_message_lines_ != previous_lines) {
        layout_dirty_ = true;
    }

    if (derived_from_animation_) {
        std::string tip;
        for (size_t i = 0; i < inherited_message_lines_.size(); ++i) {
            if (i > 0) tip.append(" ");
            tip.append(inherited_message_lines_[i]);
        }
        info_tooltip_.text = std::move(tip);
        info_tooltip_.enabled = !info_tooltip_.text.empty();
        DMWidgetTooltipResetHover(info_tooltip_);
    } else {
        info_tooltip_.enabled = false;
        info_tooltip_.text.clear();
        DMWidgetTooltipResetHover(info_tooltip_);
    }
}

}

#include "asset_info.hpp"

#include "asset_types.hpp"
#include "assets/asset/animation_loader.hpp"
#include "utils/cache_manager.hpp"
#include "utils/log.hpp"
#include "utils/oval_anchor_math.hpp"
#include "utils/utils/weighted_range.hpp"
#include "assets/asset/primary_asset_cache.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <random>
#include <limits>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <tuple>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <SDL3/SDL.h>

#include "devtools/core/manifest_store.hpp"
#include "gameplay/spawn/spawn_group_codec.hpp"
#include "utils/grid.hpp"

namespace fs = std::filesystem;

namespace {

struct CanvasMetrics {
    int width = 0;
    int height = 0;
};

std::vector<std::string> parse_string_array(const nlohmann::json& json_value) {
    std::vector<std::string> values;
    if (!json_value.is_array()) {
        return values;
    }
    values.reserve(json_value.size());
    for (const auto& entry : json_value) {
        if (entry.is_string()) {
            auto str = entry.get<std::string>();
            if (!str.empty()) {
                values.push_back(std::move(str));
            }
        }
    }
    return values;
}

constexpr const char* kAnchorPointChildCandidatesKey = "anchor_point_child_candidates";
constexpr const char* kAnchorPointChildCandidatesLegacyKey = "anchor_point_child_cndidates";
constexpr const char* kOvalAnchorMappingsKey = "oval_anchor_mappings";
constexpr double kAnchorCandidateMissingChanceDefault = 100.0;
constexpr float kDefaultOvalWidthRadius = 48.0f;
constexpr float kDefaultOvalHeightRadius = 24.0f;
constexpr float kDefaultOvalRadiusOffsetDegrees = 0.0f;
constexpr std::size_t kDefaultOvalPointCount = 8;
constexpr const char* kOvalCenterSuffix = "_oval_center";
constexpr const char* kMovementEnabledKey = "movement_enabled";
constexpr const char* kAttackBoxEnabledKey = "attack_box_enabled";
constexpr const char* kHitboxEnabledKey = "hitbox_enabled";
constexpr const char* kImpassableEnabledKey = "impassable_enabled";
constexpr const char* kImpassableShapesKey = "impassable_shapes";
constexpr const char* kFloorBoxesEnabledKey = "floor_boxes_enabled";
constexpr const char* kFloorBoxesKey = "floor_boxes";
constexpr const char* kTiltRangeKey = "tilt_range";
constexpr const char* kYPositionRangeKey = "y_position_range";
constexpr const char* kBoundaryTag = "boundary";
constexpr const char* kFloorBoxCandidateKey = "candidate";
constexpr const char* kFloorBoxCandidateCandidatesKey = "candidates";
constexpr const char* kFloorBoxCandidateGridResolutionKey = "grid_resolution";
constexpr int kFloorBoxCandidateGridResolutionMin = 2;
constexpr int kFloorBoxCandidateGridResolutionMax = 8;
constexpr int kFloorBoxCandidateGridResolutionDefault = 4;

std::string trim_copy(std::string value) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin_it = std::find_if_not(value.begin(), value.end(),
                                     [&](char c) { return is_space(static_cast<unsigned char>(c)); });
    auto end_it = std::find_if_not(value.rbegin(), value.rend(),
                                   [&](char c) { return is_space(static_cast<unsigned char>(c)); }).base();
    if (begin_it >= end_it) {
        return {};
    }
    return std::string(begin_it, end_it);
}

std::string normalize_tag_token(std::string value) {
    value = trim_copy(std::move(value));
    if (value.empty()) {
        return {};
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> normalize_tag_list(const std::vector<std::string>& raw_values) {
    std::vector<std::string> normalized;
    normalized.reserve(raw_values.size());
    std::unordered_set<std::string> seen;
    seen.reserve(raw_values.size());
    for (const std::string& raw : raw_values) {
        std::string token = normalize_tag_token(raw);
        if (token.empty()) {
            continue;
        }
        if (seen.insert(token).second) {
            normalized.push_back(std::move(token));
        }
    }
    return normalized;
}

std::vector<std::string> parse_normalized_tag_list(const nlohmann::json& payload) {
    std::vector<std::string> raw;
    if (payload.is_array()) {
        raw.reserve(payload.size());
        for (const auto& entry : payload) {
            if (!entry.is_string()) {
                continue;
            }
            raw.push_back(entry.get<std::string>());
        }
    } else if (payload.is_string()) {
        raw.push_back(payload.get<std::string>());
    }
    return normalize_tag_list(raw);
}

std::vector<std::string> strip_floor_boundary_tag(const std::vector<std::string>& tags) {
    std::vector<std::string> filtered;
    filtered.reserve(tags.size());
    for (const auto& tag : tags) {
        if (tag == kBoundaryTag) {
            continue;
        }
        filtered.push_back(tag);
    }
    return filtered;
}

std::string sanitize_floor_box_token(std::string value) {
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

std::string default_floor_box_id(std::size_t index) {
    return std::string("floor_box_") + std::to_string(index + 1);
}

std::string default_floor_box_name(std::size_t index) {
    return std::string("floor_box_") + std::to_string(index + 1);
}

float sanitize_finite_float(float value, float fallback = 0.0f) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return value;
}

float quantize_depth_offset_world_units(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return static_cast<float>(std::lround(value));
}

int sanitize_floor_box_grid_resolution(int resolution) {
    const int clamped = vibble::grid::clamp_resolution(resolution);
    return std::clamp(clamped,
                      kFloorBoxCandidateGridResolutionMin,
                      kFloorBoxCandidateGridResolutionMax);
}

std::optional<AssetInfo::FloorBox::CandidatePayload> parse_floor_box_candidate_payload(
    const nlohmann::json& floor_box_entry) {
    if (!floor_box_entry.is_object() || !floor_box_entry.contains(kFloorBoxCandidateKey)) {
        return std::nullopt;
    }

    const auto& raw_candidate = floor_box_entry[kFloorBoxCandidateKey];
    nlohmann::json candidate_entry = nlohmann::json::object();
    int grid_resolution = kFloorBoxCandidateGridResolutionDefault;

    if (raw_candidate.is_object()) {
        if (raw_candidate.contains(kFloorBoxCandidateCandidatesKey)) {
            candidate_entry[kFloorBoxCandidateCandidatesKey] =
                raw_candidate[kFloorBoxCandidateCandidatesKey];
        }
        grid_resolution = sanitize_floor_box_grid_resolution(
            vibble::spawn_group_codec::read_int_field(raw_candidate,
                                                      kFloorBoxCandidateGridResolutionKey,
                                                      kFloorBoxCandidateGridResolutionDefault));
    }

    vibble::spawn_group_codec::sanitize_spawn_group_candidates(candidate_entry);

    AssetInfo::FloorBox::CandidatePayload payload{};
    payload.candidates = candidate_entry[kFloorBoxCandidateCandidatesKey];
    payload.grid_resolution = grid_resolution;
    return payload;
}

std::optional<AssetInfo::FloorBox::CandidatePayload> sanitize_floor_box_candidate_for_save(
    const std::optional<AssetInfo::FloorBox::CandidatePayload>& raw_candidate) {
    if (!raw_candidate.has_value()) {
        return std::nullopt;
    }

    nlohmann::json candidate_entry = nlohmann::json::object();
    candidate_entry[kFloorBoxCandidateCandidatesKey] = raw_candidate->candidates;
    vibble::spawn_group_codec::sanitize_spawn_group_candidates(candidate_entry);

    AssetInfo::FloorBox::CandidatePayload payload{};
    payload.candidates = candidate_entry[kFloorBoxCandidateCandidatesKey];
    payload.grid_resolution = sanitize_floor_box_grid_resolution(raw_candidate->grid_resolution);
    return payload;
}

std::vector<AssetInfo::FloorBox> parse_floor_boxes_payload(const nlohmann::json& payload) {
    std::vector<AssetInfo::FloorBox> boxes;
    if (!payload.is_array()) {
        return boxes;
    }

    std::unordered_set<std::string> used_ids;
    boxes.reserve(payload.size());

    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto& entry = payload[index];
        if (!entry.is_object()) {
            continue;
        }

        AssetInfo::FloorBox box{};
        box.id = sanitize_floor_box_token(entry.value("id", std::string{}));
        if (box.id.empty()) {
            box.id = default_floor_box_id(index);
        }
        std::string unique_id = box.id;
        for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
            unique_id = box.id + "_" + std::to_string(suffix);
        }
        box.id = unique_id;

        box.name = trim_copy(entry.value("name", std::string{}));
        if (box.name.empty()) {
            box.name = default_floor_box_name(index);
        }

        box.position_x = sanitize_finite_float(entry.value("position_x", 0.0f), 0.0f);
        box.position_z = sanitize_finite_float(entry.value("position_z", 0.0f), 0.0f);
        box.width = std::max(0.0f, sanitize_finite_float(entry.value("width", 0.0f), 0.0f));
        box.depth = std::max(0.0f, sanitize_finite_float(entry.value("depth", 0.0f), 0.0f));
        box.enabled = entry.value("enabled", true);
        box.tags = parse_normalized_tag_list(entry.value("tags", nlohmann::json::array()));
        box.tags = strip_floor_boundary_tag(box.tags);
        box.candidate = parse_floor_box_candidate_payload(entry);

        boxes.push_back(std::move(box));
    }

    return boxes;
}

AssetInfo::FloorBox sanitize_floor_box_for_save(const AssetInfo::FloorBox& raw_box,
                                                std::size_t index,
                                                std::unordered_set<std::string>& used_ids) {
    AssetInfo::FloorBox sanitized{};

    std::string base_id = sanitize_floor_box_token(trim_copy(raw_box.id));
    if (base_id.empty()) {
        base_id = default_floor_box_id(index);
    }
    std::string unique_id = base_id;
    for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
        unique_id = base_id + "_" + std::to_string(suffix);
    }
    sanitized.id = std::move(unique_id);

    sanitized.name = trim_copy(raw_box.name);
    if (sanitized.name.empty()) {
        sanitized.name = default_floor_box_name(index);
    }

    sanitized.position_x = sanitize_finite_float(raw_box.position_x, 0.0f);
    sanitized.position_z = sanitize_finite_float(raw_box.position_z, 0.0f);
    sanitized.width = std::max(0.0f, sanitize_finite_float(raw_box.width, 0.0f));
    sanitized.depth = std::max(0.0f, sanitize_finite_float(raw_box.depth, 0.0f));
    sanitized.enabled = raw_box.enabled;
    sanitized.tags = strip_floor_boundary_tag(normalize_tag_list(raw_box.tags));
    sanitized.candidate = sanitize_floor_box_candidate_for_save(raw_box.candidate);
    return sanitized;
}

nlohmann::json encode_floor_boxes_payload(const std::vector<AssetInfo::FloorBox>& boxes) {
    nlohmann::json payload = nlohmann::json::array();
    std::unordered_set<std::string> used_ids;
    used_ids.reserve(boxes.size());
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        const AssetInfo::FloorBox box =
            sanitize_floor_box_for_save(boxes[index], index, used_ids);
        nlohmann::json entry = nlohmann::json::object();
        entry["id"] = box.id;
        entry["name"] = box.name;
        entry["position_x"] = box.position_x;
        entry["position_z"] = box.position_z;
        entry["width"] = box.width;
        entry["depth"] = box.depth;
        entry["enabled"] = box.enabled;
        entry["tags"] = box.tags;
        if (box.candidate.has_value()) {
            nlohmann::json candidate_payload = nlohmann::json::object();
            candidate_payload[kFloorBoxCandidateCandidatesKey] = box.candidate->candidates;
            candidate_payload[kFloorBoxCandidateGridResolutionKey] =
                sanitize_floor_box_grid_resolution(box.candidate->grid_resolution);
            entry[kFloorBoxCandidateKey] = std::move(candidate_payload);
        }
        payload.push_back(std::move(entry));
    }
    return payload;
}

int read_impassable_int(const nlohmann::json& node, const char* key, int fallback) {
    if (!node.is_object() || !node.contains(key)) {
        return fallback;
    }
    const auto& value = node[key];
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_float()) {
        return static_cast<int>(std::lround(value.get<double>()));
    }
    return fallback;
}

std::string default_impassable_shape_id(std::size_t index) {
    return std::string("impassable_shape_") + std::to_string(index + 1);
}

std::string default_impassable_shape_name(std::size_t index) {
    return std::string("Impassable Shape ") + std::to_string(index + 1);
}

long long polygon_signed_area_twice(const std::vector<AssetInfo::ImpassableShapePoint>& points) {
    if (points.size() < 3) {
        return 0;
    }
    long long area2 = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        area2 += static_cast<long long>(a.x) * static_cast<long long>(b.y) -
                 static_cast<long long>(b.x) * static_cast<long long>(a.y);
    }
    return area2;
}

long long segment_orientation(const AssetInfo::ImpassableShapePoint& a,
                              const AssetInfo::ImpassableShapePoint& b,
                              const AssetInfo::ImpassableShapePoint& c) {
    return (static_cast<long long>(b.x) - static_cast<long long>(a.x)) *
               (static_cast<long long>(c.y) - static_cast<long long>(a.y)) -
           (static_cast<long long>(b.y) - static_cast<long long>(a.y)) *
               (static_cast<long long>(c.x) - static_cast<long long>(a.x));
}

bool point_on_segment(const AssetInfo::ImpassableShapePoint& a,
                      const AssetInfo::ImpassableShapePoint& b,
                      const AssetInfo::ImpassableShapePoint& p) {
    if (segment_orientation(a, b, p) != 0) {
        return false;
    }
    const int min_x = std::min(a.x, b.x);
    const int max_x = std::max(a.x, b.x);
    const int min_y = std::min(a.y, b.y);
    const int max_y = std::max(a.y, b.y);
    return p.x >= min_x && p.x <= max_x && p.y >= min_y && p.y <= max_y;
}

bool segments_intersect_inclusive(const AssetInfo::ImpassableShapePoint& a1,
                                  const AssetInfo::ImpassableShapePoint& a2,
                                  const AssetInfo::ImpassableShapePoint& b1,
                                  const AssetInfo::ImpassableShapePoint& b2) {
    const long long o1 = segment_orientation(a1, a2, b1);
    const long long o2 = segment_orientation(a1, a2, b2);
    const long long o3 = segment_orientation(b1, b2, a1);
    const long long o4 = segment_orientation(b1, b2, a2);

    if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
        ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) {
        return true;
    }
    if (o1 == 0 && point_on_segment(a1, a2, b1)) {
        return true;
    }
    if (o2 == 0 && point_on_segment(a1, a2, b2)) {
        return true;
    }
    if (o3 == 0 && point_on_segment(b1, b2, a1)) {
        return true;
    }
    if (o4 == 0 && point_on_segment(b1, b2, a2)) {
        return true;
    }
    return false;
}

bool impassable_polygon_self_intersects(const std::vector<AssetInfo::ImpassableShapePoint>& points) {
    const std::size_t n = points.size();
    if (n < 4) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t i2 = (i + 1) % n;
        for (std::size_t j = i + 1; j < n; ++j) {
            const std::size_t j2 = (j + 1) % n;
            if (i == j || i == j2 || i2 == j || i2 == j2) {
                continue;
            }
            if (segments_intersect_inclusive(points[i], points[i2], points[j], points[j2])) {
                return true;
            }
        }
    }
    return false;
}

void normalize_impassable_shape_points(std::vector<AssetInfo::ImpassableShapePoint>& points) {
    if (points.size() < 3) {
        return;
    }
    if (polygon_signed_area_twice(points) < 0) {
        std::reverse(points.begin(), points.end());
    }
}

std::vector<AssetInfo::ImpassableShapePoint> parse_impassable_shape_points(const nlohmann::json& payload) {
    std::vector<AssetInfo::ImpassableShapePoint> points;
    if (!payload.is_array()) {
        return points;
    }
    points.reserve(payload.size());
    for (const auto& point_node : payload) {
        if (!point_node.is_object()) {
            continue;
        }
        const int x = read_impassable_int(point_node, "x", 0);
        const int y = read_impassable_int(point_node, "y", 0);
        points.push_back(AssetInfo::ImpassableShapePoint{x, y});
    }
    return points;
}

AssetInfo::ImpassableShape sanitize_impassable_shape_for_save(const AssetInfo::ImpassableShape& raw_shape,
                                                              std::size_t index,
                                                              std::unordered_set<std::string>& used_ids,
                                                              std::unordered_set<std::string>& used_names) {
    AssetInfo::ImpassableShape shape{};
    std::string id = sanitize_floor_box_token(trim_copy(raw_shape.id));
    if (id.empty()) {
        id = default_impassable_shape_id(index);
    }
    std::string unique_id = id;
    for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
        unique_id = id + "_" + std::to_string(suffix);
    }
    shape.id = std::move(unique_id);

    std::string name = trim_copy(raw_shape.name);
    if (name.empty()) {
        name = default_impassable_shape_name(index);
    }
    std::string unique_name = name;
    for (int suffix = 2; !used_names.insert(unique_name).second; ++suffix) {
        unique_name = name + " " + std::to_string(suffix);
    }
    shape.name = std::move(unique_name);
    shape.enabled = raw_shape.enabled;
    shape.points = raw_shape.points;
    normalize_impassable_shape_points(shape.points);
    return shape;
}

std::vector<AssetInfo::ImpassableShape> parse_impassable_shapes_payload(const nlohmann::json& payload) {
    std::vector<AssetInfo::ImpassableShape> shapes;
    if (!payload.is_array()) {
        return shapes;
    }

    std::unordered_set<std::string> used_ids;
    std::unordered_set<std::string> used_names;
    shapes.reserve(payload.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto& entry = payload[index];
        if (!entry.is_object()) {
            continue;
        }

        AssetInfo::ImpassableShape shape{};
        shape.id = sanitize_floor_box_token(entry.value("id", std::string{}));
        if (shape.id.empty()) {
            shape.id = default_impassable_shape_id(index);
        }
        std::string unique_id = shape.id;
        for (int suffix = 2; !used_ids.insert(unique_id).second; ++suffix) {
            unique_id = shape.id + "_" + std::to_string(suffix);
        }
        shape.id = std::move(unique_id);

        shape.name = trim_copy(entry.value("name", std::string{}));
        if (shape.name.empty()) {
            shape.name = default_impassable_shape_name(index);
        }
        std::string unique_name = shape.name;
        for (int suffix = 2; !used_names.insert(unique_name).second; ++suffix) {
            unique_name = shape.name + " " + std::to_string(suffix);
        }
        shape.name = std::move(unique_name);
        shape.enabled = entry.value("enabled", true);
        shape.points = parse_impassable_shape_points(entry.value("points", nlohmann::json::array()));
        normalize_impassable_shape_points(shape.points);

        if (shape.points.size() < 3) {
            continue;
        }
        if (impassable_polygon_self_intersects(shape.points)) {
            continue;
        }
        shapes.push_back(std::move(shape));
    }
    return shapes;
}

nlohmann::json encode_impassable_shapes_payload(const std::vector<AssetInfo::ImpassableShape>& shapes) {
    nlohmann::json payload = nlohmann::json::array();
    std::unordered_set<std::string> used_ids;
    std::unordered_set<std::string> used_names;
    used_ids.reserve(shapes.size());
    used_names.reserve(shapes.size());
    for (std::size_t index = 0; index < shapes.size(); ++index) {
        AssetInfo::ImpassableShape shape =
            sanitize_impassable_shape_for_save(shapes[index], index, used_ids, used_names);
        if (shape.points.size() < 3 || impassable_polygon_self_intersects(shape.points)) {
            continue;
        }
        nlohmann::json node = nlohmann::json::object();
        node["id"] = shape.id;
        node["name"] = shape.name;
        node["enabled"] = shape.enabled;
        nlohmann::json points = nlohmann::json::array();
        for (const auto& point : shape.points) {
            points.push_back(nlohmann::json::object({
                {"x", point.x},
                {"y", point.y},
            }));
        }
        node["points"] = std::move(points);
        payload.push_back(std::move(node));
    }
    return payload;
}

bool is_integral_number(double value) {
    return std::isfinite(value) && std::fabs(value - std::round(value)) <= 1e-9;
}

double clamp_non_negative_finite(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        return 0.0;
    }
    return value;
}

float clamp_size_variation_percent(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 20.0f);
}

int clamp_tilt_degrees(int value) {
    return std::clamp(value, -180, 180);
}

std::pair<int, int> tilt_range_bounds(const vibble::weighted_range::WeightedIntRange& value) {
        const std::int64_t min_raw = value.center - value.span;
        const std::int64_t max_raw = value.center + value.span;
        const auto clamp_i64_to_tilt = [](std::int64_t v) {
                if (v < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
                        return std::numeric_limits<int>::min();
                }
                if (v > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
                        return std::numeric_limits<int>::max();
                }
                return clamp_tilt_degrees(static_cast<int>(v));
        };
        int min_value = clamp_i64_to_tilt(min_raw);
        int max_value = clamp_i64_to_tilt(max_raw);
        if (min_value > max_value) {
                std::swap(min_value, max_value);
        }
        return {min_value, max_value};
}

int clamp_y_position_percent(int value) {
    return std::clamp(value, -100, 500);
}

vibble::weighted_range::WeightedIntRange sanitize_y_position_range_percent(
    const vibble::weighted_range::WeightedIntRange& range) {
    vibble::weighted_range::WeightedIntRange sanitized = range;
    sanitized.center = clamp_y_position_percent(static_cast<int>(std::clamp<std::int64_t>(
        sanitized.center,
        static_cast<std::int64_t>(std::numeric_limits<int>::min()),
        static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    sanitized.span = std::max<std::int64_t>(0, sanitized.span);
    if (sanitized.center - sanitized.span < -100) {
        sanitized.span = sanitized.center + 100;
    }
    if (sanitized.center + sanitized.span > 500) {
        sanitized.span = std::min<std::int64_t>(sanitized.span, 500 - sanitized.center);
    }
    sanitized.span = std::max<std::int64_t>(0, sanitized.span);
    return sanitized;
}

std::optional<double> parse_number_like_json(const nlohmann::json& value) {
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_number_integer()) {
        return static_cast<double>(value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return static_cast<double>(value.get<std::uint64_t>());
    }
    if (value.is_string()) {
        const std::string text = trim_copy(value.get<std::string>());
        if (text.empty()) {
            return std::nullopt;
        }
        try {
            std::size_t idx = 0;
            const double parsed = std::stod(text, &idx);
            if (idx == text.size()) {
                return parsed;
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

vibble::weighted_range::WeightedIntRange read_weighted_range_field(
    const nlohmann::json& data,
    const char* key,
    const vibble::weighted_range::WeightedIntRange& fallback = vibble::weighted_range::make_flat(0)) {
    if (!data.is_object() || !data.contains(key)) {
        return fallback;
    }
    return vibble::weighted_range::from_json(data.at(key), fallback);
}

vibble::weighted_range::WeightedIntRange read_weighted_range_legacy_pair(
    const nlohmann::json& data,
    const char* min_key,
    const char* max_key,
    const vibble::weighted_range::WeightedIntRange& fallback = vibble::weighted_range::make_flat(0)) {
    if (!data.is_object()) {
        return fallback;
    }
    const auto min_it = data.find(min_key);
    const auto max_it = data.find(max_key);
    if (min_it == data.end() && max_it == data.end()) {
        return fallback;
    }
    const auto min_value = min_it != data.end() ? parse_number_like_json(*min_it) : std::nullopt;
    const auto max_value = max_it != data.end() ? parse_number_like_json(*max_it) : std::nullopt;
    if (min_value || max_value) {
        const std::int64_t min_i = static_cast<std::int64_t>(std::llround(min_value.value_or(fallback.center)));
        const std::int64_t max_i = static_cast<std::int64_t>(std::llround(max_value.value_or(min_i)));
        return vibble::weighted_range::make_legacy_uniform(min_i, max_i);
    }
    return fallback;
}

std::string ensure_tag_prefix(std::string value) {
    value = trim_copy(std::move(value));
    if (!value.empty() && value.front() != '#') {
        value.insert(value.begin(), '#');
    }
    return value;
}

bool is_vibble_asset_name(const std::string& raw_name) {
    std::string lowered = trim_copy(raw_name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered == "vibble";
}

std::string normalize_anchor_candidate_name(const nlohmann::json& candidate_json,
                                            const std::string& legacy_key = std::string{}) {
    std::string name = trim_copy(legacy_key);
    std::string explicit_tag_name;
    bool tag_flag = false;

    if (candidate_json.is_object()) {
        if (candidate_json.contains("name") && candidate_json["name"].is_string()) {
            name = trim_copy(candidate_json["name"].get<std::string>());
        } else if (candidate_json.contains("asset_name") && candidate_json["asset_name"].is_string()) {
            name = trim_copy(candidate_json["asset_name"].get<std::string>());
        }

        if (candidate_json.contains("tag_name") && candidate_json["tag_name"].is_string()) {
            explicit_tag_name = trim_copy(candidate_json["tag_name"].get<std::string>());
        }

        if (candidate_json.contains("tag")) {
            const auto& tag_value = candidate_json["tag"];
            if (tag_value.is_boolean()) {
                tag_flag = tag_value.get<bool>();
            } else if (tag_value.is_string()) {
                explicit_tag_name = trim_copy(tag_value.get<std::string>());
            }
        }
    }

    if (!explicit_tag_name.empty()) {
        name = ensure_tag_prefix(explicit_tag_name);
    } else if (tag_flag && !name.empty()) {
        name = ensure_tag_prefix(name);
    }

    return name;
}

nlohmann::json encode_anchor_candidate_entry(const std::string& name, double chance) {
    nlohmann::json candidate = nlohmann::json::object();
    candidate["name"] = name.empty() ? std::string("null") : name;
    chance = clamp_non_negative_finite(chance);
    if (is_integral_number(chance)) {
        candidate["chance"] = static_cast<int>(std::llround(chance));
    } else {
        candidate["chance"] = chance;
    }
    return candidate;
}

nlohmann::json normalize_single_anchor_candidate(const nlohmann::json& candidate_json,
                                                 const std::string& legacy_key = std::string{}) {
    const std::string normalized_name = normalize_anchor_candidate_name(candidate_json, legacy_key);

    bool had_explicit_weight = false;
    double chance = 0.0;
    if (candidate_json.is_object()) {
        if (candidate_json.contains("chance") || candidate_json.contains("weight")) {
            had_explicit_weight = true;
            chance = vibble::spawn_group_codec::read_candidate_chance(candidate_json, 0.0);
        }
    } else {
        const auto parsed = parse_number_like_json(candidate_json);
        if (parsed.has_value()) {
            had_explicit_weight = true;
            chance = *parsed;
        }
    }

    if (!had_explicit_weight) {
        chance = normalized_name.empty() ? 0.0 : kAnchorCandidateMissingChanceDefault;
    }
    return encode_anchor_candidate_entry(normalized_name, chance);
}

nlohmann::json normalize_anchor_candidate_payload(const nlohmann::json& payload) {
    nlohmann::json normalized = nlohmann::json::object();
    normalized["candidates"] = nlohmann::json::array();
    auto& normalized_candidates = normalized["candidates"];

    if (payload.is_object()) {
        auto candidates_it = payload.find("candidates");
        if (candidates_it != payload.end() && candidates_it->is_array()) {
            for (const auto& candidate_json : *candidates_it) {
                normalized_candidates.push_back(normalize_single_anchor_candidate(candidate_json));
            }
        } else {
            for (auto it = payload.begin(); it != payload.end(); ++it) {
                normalized_candidates.push_back(normalize_single_anchor_candidate(it.value(), it.key()));
            }
        }
    } else if (payload.is_array()) {
        for (const auto& candidate_json : payload) {
            normalized_candidates.push_back(normalize_single_anchor_candidate(candidate_json));
        }
    } else if (!payload.is_null()) {
        normalized_candidates.push_back(normalize_single_anchor_candidate(payload));
    }

    return normalized;
}

std::vector<AssetInfo::AnchorChildPointCandidate> parse_anchor_point_child_candidates_payload(
    const nlohmann::json& payload) {
    std::vector<AssetInfo::AnchorChildPointCandidate> parsed;
    if (!payload.is_array()) {
        return parsed;
    }

    std::unordered_set<std::string> seen_names;
    parsed.reserve(payload.size());
    for (const auto& entry : payload) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string anchor_point_name = entry.value("anchor_point_name", std::string{});
        if (anchor_point_name.empty()) {
            continue;
        }
        if (!seen_names.insert(anchor_point_name).second) {
            continue;
        }

        AssetInfo::AnchorChildPointCandidate candidate{};
        candidate.anchor_point_name = anchor_point_name;
        if (entry.contains("candidates")) {
            candidate.candidates = normalize_anchor_candidate_payload(entry["candidates"]);
        } else {
            candidate.candidates = normalize_anchor_candidate_payload(nlohmann::json::object());
        }
        candidate.orphan_on_end = entry.value("orphan_on_end", true);
        parsed.push_back(std::move(candidate));
    }

    return parsed;
}

nlohmann::json build_anchor_point_child_candidates_payload(
    const std::vector<AssetInfo::AnchorChildPointCandidate>& candidates) {
    nlohmann::json payload = nlohmann::json::array();
    std::unordered_set<std::string> seen_names;
    for (const auto& candidate : candidates) {
        if (candidate.anchor_point_name.empty()) {
            continue;
        }
        if (!seen_names.insert(candidate.anchor_point_name).second) {
            continue;
        }
        nlohmann::json encoded = nlohmann::json::object();
        encoded["anchor_point_name"] = candidate.anchor_point_name;
        encoded["candidates"] = normalize_anchor_candidate_payload(candidate.candidates);
        encoded["orphan_on_end"] = candidate.orphan_on_end;
        payload.push_back(std::move(encoded));
    }
    return payload;
}

float normalize_angle_degrees(float angle_degrees) {
    return oval_anchor_math::normalize_angle_degrees(angle_degrees);
}

float sanitize_oval_radius(float radius, float fallback) {
    if (!std::isfinite(radius) || radius <= 0.0f) {
        return fallback;
    }
    return radius;
}

float sanitize_oval_radius_offset_degrees(float value) {
    if (!std::isfinite(value)) {
        return kDefaultOvalRadiusOffsetDegrees;
    }
    return std::clamp(value, 0.0f, 360.0f);
}

std::string oval_center_anchor_name(std::string mapping_name) {
    mapping_name = trim_copy(std::move(mapping_name));
    if (mapping_name.empty()) {
        return {};
    }
    return mapping_name + kOvalCenterSuffix;
}

void recompute_oval_point_position(AssetInfo::OvalAnchorPoint& point,
                                   float width_radius_x,
                                   float height_radius_z) {
    point.angle_degrees = normalize_angle_degrees(point.angle_degrees);
    oval_anchor_math::compute_xz_offsets_from_angle(point.angle_degrees,
                                                    width_radius_x,
                                                    height_radius_z,
                                                    point.texture_x,
                                                    point.texture_y);
    if (!std::isfinite(point.depth_offset)) {
        point.depth_offset = 0.0f;
    }
}

void normalize_and_sort_oval_points(std::vector<AssetInfo::OvalAnchorPoint>& points) {
    for (auto& point : points) {
        point.angle_degrees = normalize_angle_degrees(point.angle_degrees);
        if (!std::isfinite(point.rotation_degrees)) {
            point.rotation_degrees = 0.0f;
        }
        if (!std::isfinite(point.depth_offset)) {
            point.depth_offset = 0.0f;
        }
        point.tags = normalize_tag_list(point.tags);
        point.anti_tags = normalize_tag_list(point.anti_tags);
    }

    std::sort(points.begin(),
              points.end(),
              [](const AssetInfo::OvalAnchorPoint& lhs, const AssetInfo::OvalAnchorPoint& rhs) {
                  return normalize_angle_degrees(lhs.angle_degrees) < normalize_angle_degrees(rhs.angle_degrees);
              });

    constexpr float kDuplicateAngleEpsilon = 1e-3f;
    std::vector<AssetInfo::OvalAnchorPoint> deduped;
    deduped.reserve(points.size());
    for (const auto& point : points) {
        if (deduped.empty()) {
            deduped.push_back(point);
            continue;
        }
        const float current_angle = normalize_angle_degrees(point.angle_degrees);
        const float previous_angle = normalize_angle_degrees(deduped.back().angle_degrees);
        if (std::fabs(current_angle - previous_angle) <= kDuplicateAngleEpsilon) {
            continue;
        }
        deduped.push_back(point);
    }
    points = std::move(deduped);
}

float read_point_angle_for_legacy_detection(const nlohmann::json& payload,
                                            std::size_t fallback_index,
                                            std::size_t fallback_count) {
    float angle = normalize_angle_degrees(
        (360.0f * static_cast<float>(fallback_index)) /
        static_cast<float>(std::max<std::size_t>(1, fallback_count)));
    if (!payload.is_object()) {
        return angle;
    }
    if (payload.contains("angle_degrees")) {
        if (const auto parsed = parse_number_like_json(payload["angle_degrees"])) {
            angle = normalize_angle_degrees(static_cast<float>(*parsed));
        }
    }
    return angle;
}

bool points_payload_looks_legacy_x_depth_model(const nlohmann::json& points_payload,
                                               float height_radius_z) {
    if (!points_payload.is_array() || points_payload.empty()) {
        return false;
    }
    const std::size_t point_count = std::max<std::size_t>(1, points_payload.size());
    int comparable_points = 0;
    int matched_points = 0;
    for (std::size_t idx = 0; idx < point_count; ++idx) {
        const auto& entry = points_payload[idx];
        if (!entry.is_object()) {
            continue;
        }
        const float angle = read_point_angle_for_legacy_detection(entry, idx, point_count);
        const int expected_offset_z = [&]() {
            int offset_x = 0;
            int offset_z = 0;
            oval_anchor_math::compute_xz_offsets_from_angle(angle,
                                                            1.0f,
                                                            height_radius_z,
                                                            offset_x,
                                                            offset_z);
            return offset_z;
        }();
        if (std::abs(expected_offset_z) <= 1) {
            continue;
        }
        ++comparable_points;

        const float raw_texture_y = entry.contains("texture_y")
            ? static_cast<float>(parse_number_like_json(entry["texture_y"]).value_or(0.0))
            : 0.0f;
        const float raw_depth_offset = entry.contains("depth_offset")
            ? static_cast<float>(parse_number_like_json(entry["depth_offset"]).value_or(0.0))
            : 0.0f;
        const bool texture_y_legacy = std::fabs(raw_texture_y) <= 0.5f;
        const bool depth_matches_legacy =
            std::fabs(raw_depth_offset - static_cast<float>(expected_offset_z)) <= 0.75f;
        if (texture_y_legacy && depth_matches_legacy) {
            ++matched_points;
        }
    }
    return comparable_points > 0 && matched_points == comparable_points;
}

AssetInfo::OvalAnchorPoint make_default_oval_anchor_point(std::size_t point_index,
                                                          std::size_t point_count,
                                                          int center_texture_x,
                                                          int center_texture_y,
                                                          float width_radius_x,
                                                          float height_radius_z) {
    (void)center_texture_x;
    (void)center_texture_y;
    AssetInfo::OvalAnchorPoint point{};
    const std::size_t clamped_count = std::max<std::size_t>(1, point_count);
    point.angle_degrees =
        normalize_angle_degrees((360.0f * static_cast<float>(point_index)) / static_cast<float>(clamped_count));
    recompute_oval_point_position(point, width_radius_x, height_radius_z);
    point.rotation_degrees = 0.0f;
    point.hidden = false;
    point.resolve_x = true;
    point.scaling_method = AnchorScalingMethod::Parent;
    return point;
}

std::vector<AssetInfo::OvalAnchorPoint> make_default_oval_anchor_points(int center_texture_x,
                                                                         int center_texture_y,
                                                                         float width_radius_x,
                                                                         float height_radius_z,
                                                                         std::size_t count = kDefaultOvalPointCount) {
    const std::size_t point_count = std::max<std::size_t>(1, count);
    std::vector<AssetInfo::OvalAnchorPoint> points;
    points.reserve(point_count);
    for (std::size_t i = 0; i < point_count; ++i) {
        points.push_back(make_default_oval_anchor_point(i,
                                                        point_count,
                                                        center_texture_x,
                                                        center_texture_y,
                                                        width_radius_x,
                                                        height_radius_z));
    }
    return points;
}

AssetInfo::OvalAnchorPoint normalize_oval_anchor_point(const nlohmann::json& payload,
                                                       std::size_t fallback_index,
                                                       std::size_t fallback_count,
                                                       int center_texture_x,
                                                       int center_texture_y,
                                                       float width_radius_x,
                                                       float height_radius_z,
                                                       bool legacy_x_depth_model) {
    (void)center_texture_x;
    (void)center_texture_y;
    AssetInfo::OvalAnchorPoint point =
        make_default_oval_anchor_point(fallback_index,
                                       fallback_count,
                                       center_texture_x,
                                       center_texture_y,
                                       width_radius_x,
                                       height_radius_z);

    if (!payload.is_object()) {
        return point;
    }

    if (payload.contains("angle_degrees")) {
        if (const auto parsed_angle = parse_number_like_json(payload["angle_degrees"])) {
            point.angle_degrees = normalize_angle_degrees(static_cast<float>(*parsed_angle));
        }
    }
    if (payload.contains("rotation_degrees")) {
        if (const auto parsed = parse_number_like_json(payload["rotation_degrees"])) {
            point.rotation_degrees = std::isfinite(*parsed) ? static_cast<float>(*parsed) : 0.0f;
        }
    }
    if (!legacy_x_depth_model && payload.contains("depth_offset")) {
        if (const auto parsed = parse_number_like_json(payload["depth_offset"])) {
            point.depth_offset = std::isfinite(*parsed)
                ? quantize_depth_offset_world_units(static_cast<float>(*parsed))
                : 0.0f;
        }
    } else {
        point.depth_offset = 0.0f;
    }
    if (payload.contains("hidden") && payload["hidden"].is_boolean()) {
        point.hidden = payload["hidden"].get<bool>();
    }
    if (payload.contains("resolve_x") && payload["resolve_x"].is_boolean()) {
        point.resolve_x = payload["resolve_x"].get<bool>();
    }
    if (payload.contains("flip_horizontal") || payload.contains("flip_vertical")) {
        throw std::runtime_error(
            "oval point payload contains removed inversion fields 'flip_horizontal'/'flip_vertical'");
    }
    if (payload.contains("scaling_method") && payload["scaling_method"].is_string()) {
        point.scaling_method = anchor_points::anchor_scaling_method_from_token(
            payload["scaling_method"].get<std::string>(),
            AnchorScalingMethod::Parent);
    }
    if (payload.contains("tags")) {
        point.tags = parse_normalized_tag_list(payload["tags"]);
    }
    if (payload.contains("anti_tags")) {
        point.anti_tags = parse_normalized_tag_list(payload["anti_tags"]);
    }
    recompute_oval_point_position(point, width_radius_x, height_radius_z);
    return point;
}

std::vector<AssetInfo::OvalAnchorMapping> parse_oval_anchor_mappings_payload(const nlohmann::json& payload,
                                                                              const std::string& fallback_asset_name = std::string{}) {
    std::vector<AssetInfo::OvalAnchorMapping> parsed;
    if (!payload.is_array()) {
        return parsed;
    }

    std::unordered_set<std::string> seen_names;
    parsed.reserve(payload.size());
    for (const auto& entry : payload) {
        if (!entry.is_object()) {
            continue;
        }
        AssetInfo::OvalAnchorMapping mapping{};
        mapping.name = trim_copy(entry.value("name", std::string{}));
        if (mapping.name.empty()) {
            continue;
        }
        if (!seen_names.insert(mapping.name).second) {
            continue;
        }
        mapping.asset_name = trim_copy(entry.value("asset_name", std::string{}));
        if (mapping.asset_name.empty()) {
            mapping.asset_name = fallback_asset_name.empty() ? mapping.name : fallback_asset_name;
        }
        const auto width_value = parse_number_like_json(entry.value("width_radius_x", nlohmann::json{}));
        const auto height_value = parse_number_like_json(entry.value("height_radius_z", nlohmann::json{}));
        mapping.width_radius_x = sanitize_oval_radius(width_value ? static_cast<float>(*width_value) : kDefaultOvalWidthRadius,
                                                      kDefaultOvalWidthRadius);
        mapping.height_radius_z =
            sanitize_oval_radius(height_value ? static_cast<float>(*height_value) : kDefaultOvalHeightRadius,
                                 kDefaultOvalHeightRadius);
        const auto offset_value = parse_number_like_json(entry.value("radius_offset_degrees", nlohmann::json{}));
        mapping.radius_offset_degrees = sanitize_oval_radius_offset_degrees(
            offset_value ? static_cast<float>(*offset_value) : kDefaultOvalRadiusOffsetDegrees);

        std::unordered_set<std::string> seen_legacy;
        if (entry.contains("legacy_names") && entry["legacy_names"].is_array()) {
            for (const auto& legacy : entry["legacy_names"]) {
                if (!legacy.is_string()) {
                    continue;
                }
                std::string legacy_name = trim_copy(legacy.get<std::string>());
                if (legacy_name.empty() || legacy_name == mapping.name) {
                    continue;
                }
                if (seen_legacy.insert(legacy_name).second) {
                    mapping.legacy_names.push_back(std::move(legacy_name));
                }
            }
        }

        const auto points_it = entry.find("points");
        const bool legacy_x_depth_model =
            points_it != entry.end() &&
            points_it->is_array() &&
            points_payload_looks_legacy_x_depth_model(*points_it, mapping.height_radius_z);
        if (points_it != entry.end() && points_it->is_array()) {
            const std::size_t point_count = std::max<std::size_t>(1, points_it->size());
            mapping.points.reserve(point_count);
            std::size_t idx = 0;
            for (const auto& point_entry : *points_it) {
                mapping.points.push_back(normalize_oval_anchor_point(point_entry,
                                                                     idx,
                                                                     point_count,
                                                                     0,
                                                                     0,
                                                                     mapping.width_radius_x,
                                                                     mapping.height_radius_z,
                                                                     legacy_x_depth_model));
                ++idx;
            }
        }
        if (mapping.points.empty()) {
            mapping.points =
                make_default_oval_anchor_points(0, 0, mapping.width_radius_x, mapping.height_radius_z, kDefaultOvalPointCount);
        }
        normalize_and_sort_oval_points(mapping.points);
        if (mapping.points.empty()) {
            mapping.points =
                make_default_oval_anchor_points(0, 0, mapping.width_radius_x, mapping.height_radius_z, kDefaultOvalPointCount);
        }
        parsed.push_back(std::move(mapping));
    }
    return parsed;
}

nlohmann::json encode_oval_anchor_point(const AssetInfo::OvalAnchorPoint& point) {
    nlohmann::json encoded = nlohmann::json::object();
    encoded["angle_degrees"] = normalize_angle_degrees(point.angle_degrees);
    encoded["texture_x"] = point.texture_x;
    encoded["texture_y"] = point.texture_y;
    encoded["depth_offset"] = quantize_depth_offset_world_units(point.depth_offset);
    encoded["rotation_degrees"] = std::isfinite(point.rotation_degrees) ? point.rotation_degrees : 0.0f;
    encoded["hidden"] = point.hidden;
    encoded["resolve_x"] = point.resolve_x;
    encoded["scaling_method"] = std::string(anchor_points::anchor_scaling_method_to_token(point.scaling_method));
    encoded["tags"] = normalize_tag_list(point.tags);
    encoded["anti_tags"] = normalize_tag_list(point.anti_tags);
    return encoded;
}

nlohmann::json build_oval_anchor_mappings_payload(const std::vector<AssetInfo::OvalAnchorMapping>& mappings) {
    nlohmann::json payload = nlohmann::json::array();
    std::unordered_set<std::string> seen_names;
    for (const auto& mapping : mappings) {
        if (!mapping.valid()) {
            continue;
        }
        if (!seen_names.insert(mapping.name).second) {
            continue;
        }
        nlohmann::json encoded = nlohmann::json::object();
        encoded["name"] = mapping.name;
        encoded["asset_name"] = mapping.asset_name.empty() ? mapping.name : mapping.asset_name;
        encoded["width_radius_x"] = sanitize_oval_radius(mapping.width_radius_x, kDefaultOvalWidthRadius);
        encoded["height_radius_z"] = sanitize_oval_radius(mapping.height_radius_z, kDefaultOvalHeightRadius);
        encoded["radius_offset_degrees"] = sanitize_oval_radius_offset_degrees(mapping.radius_offset_degrees);

        nlohmann::json legacy_names = nlohmann::json::array();
        std::unordered_set<std::string> seen_legacy;
        for (const auto& legacy_name : mapping.legacy_names) {
            if (legacy_name.empty() || legacy_name == mapping.name) {
                continue;
            }
            if (seen_legacy.insert(legacy_name).second) {
                legacy_names.push_back(legacy_name);
            }
        }
        encoded["legacy_names"] = std::move(legacy_names);

        nlohmann::json points = nlohmann::json::array();
        if (mapping.points.empty()) {
            for (const auto& point :
                 make_default_oval_anchor_points(0,
                                                 0,
                                                 sanitize_oval_radius(mapping.width_radius_x, kDefaultOvalWidthRadius),
                                                 sanitize_oval_radius(mapping.height_radius_z, kDefaultOvalHeightRadius),
                                                 kDefaultOvalPointCount)) {
                points.push_back(encode_oval_anchor_point(point));
            }
        } else {
            for (const auto& point : mapping.points) {
                points.push_back(encode_oval_anchor_point(point));
            }
        }
        encoded["points"] = std::move(points);
        payload.push_back(std::move(encoded));
    }
    return payload;
}

SDL_Point resolve_anchor_frame_dimensions(const AssetInfo& info, const AnimationFrame* frame) {
    int frame_w = 0;
    int frame_h = 0;

    if (frame && !frame->variants.empty()) {
        const FrameVariant& variant = frame->variants.front();
        if (variant.source_rect.w > 0 && variant.source_rect.h > 0) {
            frame_w = variant.source_rect.w;
            frame_h = variant.source_rect.h;
        } else if (SDL_Texture* texture = variant.get_base_texture()) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            if (SDL_GetTextureSize(texture, &tex_w, &tex_h)) {
                frame_w = static_cast<int>(std::lround(tex_w));
                frame_h = static_cast<int>(std::lround(tex_h));
            }
        }
    }

    if (frame_w <= 0) {
        frame_w = std::max(1, info.original_canvas_width);
    }
    if (frame_h <= 0) {
        frame_h = std::max(1, info.original_canvas_height);
    }
    return SDL_Point{std::max(1, frame_w), std::max(1, frame_h)};
}

DisplacedAssetAnchorPoint make_default_center_anchor(const std::string& center_anchor_name,
                                                     const SDL_Point& frame_dims) {
    const int center_x = std::max(1, frame_dims.x) / 2;
    const int center_y = std::max(1, frame_dims.y) - 1;
    return DisplacedAssetAnchorPoint{center_anchor_name, center_x, center_y, 0.0f};
}

nlohmann::json encode_anchor_point_json(const DisplacedAssetAnchorPoint& anchor) {
    nlohmann::json encoded = nlohmann::json::object();
    encoded["name"] = anchor.name;
    encoded["texture_x"] = anchor.texture_x;
    encoded["texture_y"] = anchor.texture_y;
    encoded["depth_offset"] = quantize_depth_offset_world_units(anchor.depth_offset);
    encoded["flip_horizontal"] = anchor.flip_horizontal;
    encoded["flip_vertical"] = anchor.flip_vertical;
    encoded["rotation_degrees"] = std::isfinite(anchor.rotation_degrees) ? anchor.rotation_degrees : 0.0f;
    encoded["hidden"] = anchor.hidden;
    encoded["resolve_x"] = anchor.resolve_x;
    encoded["scaling_method"] = std::string(anchor_points::anchor_scaling_method_to_token(anchor.scaling_method));
    if (anchor.has_light_data) {
        nlohmann::json light_json = nlohmann::json::object();
        light_json["enabled"] = anchor.light.enabled;
        light_json["color"] = nlohmann::json::array(
            {anchor.light.color_r, anchor.light.color_g, anchor.light.color_b});
        light_json["opacity"] = anchor.light.opacity;
        light_json["intensity"] = anchor.light.intensity;
        light_json["radius"] = anchor.light.radius;
        light_json["falloff"] = anchor.light.falloff;
        light_json["shadow_strength"] = anchor.light.shadow_strength;
        light_json["cast_shadows"] = anchor.light.cast_shadows;
        encoded["light"] = std::move(light_json);
    }
    return encoded;
}

nlohmann::json encode_anchor_frame_json(const std::vector<DisplacedAssetAnchorPoint>& anchors) {
    nlohmann::json frame_json = nlohmann::json::array();
    for (const auto& anchor : anchors) {
        if (anchor.is_valid()) {
            frame_json.push_back(encode_anchor_point_json(anchor));
        }
    }
    return frame_json;
}

void normalize_anchor_points_payload(nlohmann::json& animation_payload, std::size_t frame_count) {
    if (!animation_payload.is_object()) {
        animation_payload = nlohmann::json::object();
    }
    nlohmann::json anchor_points = nlohmann::json::array();
    auto existing_it = animation_payload.find("anchor_points");
    if (existing_it != animation_payload.end() && existing_it->is_array()) {
        anchor_points = *existing_it;
    }

    nlohmann::json normalized = nlohmann::json::array();
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (frame_index < anchor_points.size() && anchor_points[frame_index].is_array()) {
            normalized.push_back(anchor_points[frame_index]);
        } else {
            normalized.push_back(nlohmann::json::array());
        }
    }
    animation_payload["anchor_points"] = std::move(normalized);
}

bool write_anchor_frame_to_animation_payload(nlohmann::json& animation_payload,
                                             std::size_t frame_count,
                                             std::size_t frame_index,
                                             const std::vector<DisplacedAssetAnchorPoint>& anchors) {
    if (frame_count == 0 || frame_index >= frame_count) {
        return false;
    }
    normalize_anchor_points_payload(animation_payload, frame_count);
    auto it = animation_payload.find("anchor_points");
    if (it == animation_payload.end() || !it->is_array() || frame_index >= it->size()) {
        return false;
    }
    (*it)[frame_index] = encode_anchor_frame_json(anchors);
    return true;
}

bool upsert_anchor_in_frame(std::vector<DisplacedAssetAnchorPoint>& anchors,
                            const DisplacedAssetAnchorPoint& anchor) {
    if (!anchor.is_valid()) {
        return false;
    }
    for (auto& existing : anchors) {
        if (existing.name == anchor.name) {
            return false;
        }
    }
    anchors.push_back(anchor);
    return true;
}

bool rename_anchor_in_frame(std::vector<DisplacedAssetAnchorPoint>& anchors,
                            const std::string& old_name,
                            const std::string& new_name) {
    if (old_name.empty() || new_name.empty() || old_name == new_name) {
        return false;
    }
    bool changed = false;
    bool has_new = false;
    for (const auto& anchor : anchors) {
        if (anchor.name == new_name) {
            has_new = true;
            break;
        }
    }

    for (auto it = anchors.begin(); it != anchors.end();) {
        if (it->name != old_name) {
            ++it;
            continue;
        }
        if (has_new) {
            it = anchors.erase(it);
            changed = true;
            continue;
        }
        it->name = new_name;
        has_new = true;
        changed = true;
        ++it;
    }
    return changed;
}

bool upsert_anchor_in_frame_json(nlohmann::json& frame_anchor_array,
                                 const DisplacedAssetAnchorPoint& anchor) {
    if (!anchor.is_valid()) {
        return false;
    }
    if (!frame_anchor_array.is_array()) {
        frame_anchor_array = nlohmann::json::array();
    }
    for (const auto& entry : frame_anchor_array) {
        if (!entry.is_object()) {
            continue;
        }
        if (entry.value("name", std::string{}) == anchor.name) {
            return false;
        }
    }
    frame_anchor_array.push_back(encode_anchor_point_json(anchor));
    return true;
}

bool rename_anchor_in_frame_json(nlohmann::json& frame_anchor_array,
                                 const std::string& old_name,
                                 const std::string& new_name) {
    if (old_name.empty() || new_name.empty() || old_name == new_name) {
        return false;
    }
    if (!frame_anchor_array.is_array()) {
        return false;
    }

    bool changed = false;
    bool has_new = false;
    for (const auto& entry : frame_anchor_array) {
        if (entry.is_object() && entry.value("name", std::string{}) == new_name) {
            has_new = true;
            break;
        }
    }

    for (auto it = frame_anchor_array.begin(); it != frame_anchor_array.end();) {
        if (!it->is_object() || it->value("name", std::string{}) != old_name) {
            ++it;
            continue;
        }
        if (has_new) {
            it = frame_anchor_array.erase(it);
            changed = true;
            continue;
        }
        (*it)["name"] = new_name;
        has_new = true;
        changed = true;
        ++it;
    }
    return changed;
}

bool remove_anchor_in_frame(std::vector<DisplacedAssetAnchorPoint>& anchors,
                            const std::unordered_set<std::string>& names) {
    if (anchors.empty() || names.empty()) {
        return false;
    }
    const auto erase_it = std::remove_if(anchors.begin(),
                                         anchors.end(),
                                         [&](const DisplacedAssetAnchorPoint& anchor) {
                                             return names.find(anchor.name) != names.end();
                                         });
    if (erase_it == anchors.end()) {
        return false;
    }
    anchors.erase(erase_it, anchors.end());
    return true;
}

bool remove_anchor_in_frame_json(nlohmann::json& frame_anchor_array,
                                 const std::unordered_set<std::string>& names) {
    if (!frame_anchor_array.is_array() || names.empty()) {
        return false;
    }
    bool changed = false;
    for (auto it = frame_anchor_array.begin(); it != frame_anchor_array.end();) {
        if (!it->is_object()) {
            ++it;
            continue;
        }
        const std::string anchor_name = it->value("name", std::string{});
        if (names.find(anchor_name) == names.end()) {
            ++it;
            continue;
        }
        it = frame_anchor_array.erase(it);
        changed = true;
    }
    return changed;
}

const nlohmann::json* locate_animation_payloads(const nlohmann::json& root);

nlohmann::json normalize_animation_payload(nlohmann::json payload) {
    if (!payload.is_object()) {
        return nlohmann::json::object();
    }
    payload.erase("speed");
    payload.erase("speed_factor");
    payload.erase("speed_multiplier");
    payload.erase("fps");
    payload.erase("loop");

    std::string on_end = "default";
    if (payload.contains("on_end")) {
        if (payload["on_end"].is_string()) {
            on_end = payload["on_end"].get<std::string>();
        } else if (payload["on_end"].is_null()) {
            on_end = "default";
        }
    }
    if (on_end.empty()) {
        on_end = "default";
    }
    std::string lowered = on_end;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "default" || lowered == "loop" || lowered == "kill" || lowered == "lock" ||
        lowered == "reverse") {
        payload["on_end"] = lowered;
    } else {
        payload["on_end"] = on_end;
    }

    return payload;
}

void apply_system_enable_flags_to_animation_payload(nlohmann::json& payload,
                                                    bool movement_enabled,
                                                    bool hitbox_enabled,
                                                    bool attack_box_enabled) {
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
        return;
    }

    if (!movement_enabled) {
        payload.erase("movement");
        payload.erase("movement_paths");
        payload.erase("movement_total");
    }
    if (!hitbox_enabled) {
        payload.erase("hit_boxes");
    }
    if (!attack_box_enabled) {
        payload.erase("attack_boxes");
    }
}

std::string source_value_string(const nlohmann::json& payload, const char* key) {
    if (!payload.is_object()) {
        return {};
    }
    if (!payload.contains("source") || !payload["source"].is_object()) {
        return {};
    }
    const nlohmann::json& source = payload["source"];
    if (!source.contains(key) || source[key].is_null()) {
        return {};
    }
    try {
        if (source[key].is_string()) {
            return source[key].get<std::string>();
        }
        return source[key].dump();
    } catch (...) {
        return {};
    }
}

const nlohmann::json* snapshot_animations_object(const nlohmann::json& snapshot) {
    if (!snapshot.is_object()) {
        return nullptr;
    }
    auto it = snapshot.find("animations");
    if (it == snapshot.end() || !it->is_object()) {
        return nullptr;
    }
    return &(*it);
}

std::filesystem::path assets_root_for(const std::string& asset_name) {
    std::filesystem::path base = std::filesystem::path("resources") / "assets";
    if (!asset_name.empty()) {
        base /= asset_name;
    }
    return base.lexically_normal();
}

bool path_starts_with_resources(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const std::string generic = path.lexically_normal().generic_string();
    return generic == "resources" || generic.rfind("resources/", 0) == 0;
}

std::string prefer_assets_directory(const std::string& configured, const std::string& asset_name) {
    const auto preferred = assets_root_for(asset_name);

    if (configured.empty()) {
        return preferred.generic_string();
    }

    std::filesystem::path candidate = std::filesystem::path(configured).lexically_normal();
    if (!path_starts_with_resources(candidate)) {
        return candidate.generic_string();
    }

    if (candidate == preferred) {
        return candidate.generic_string();
    }

    std::error_code ec;
    const bool preferred_exists = std::filesystem::exists(preferred, ec);
    ec.clear();
    const bool candidate_exists = std::filesystem::exists(candidate, ec);
    ec.clear();
    if (candidate_exists) {
        return candidate.generic_string();
    }

    if (preferred_exists) {
        return preferred.generic_string();
    }

    return preferred.generic_string();
}

std::string derive_asset_directory(const nlohmann::json& data, const std::string& fallback) {
    try {
        if (data.contains("asset_directory") && data["asset_directory"].is_string()) {
            auto value = data["asset_directory"].get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }

        auto anims_it = data.find("animations");
        if (anims_it != data.end() && anims_it->is_object()) {
            for (auto it = anims_it->begin(); it != anims_it->end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }
                const auto& anim_json = it.value();
                if (anim_json.contains("source") && anim_json["source"].is_object()) {
                    std::string path = anim_json["source"].value("path", std::string{});
                    if (!path.empty()) {
                        std::filesystem::path p(path);
                        if (!p.empty()) {
                            if (p.has_filename()) {
                                p = p.parent_path();
                            }
                            return p.string();
                        }
                    }
                } else if (anim_json.contains("frames_path") && anim_json["frames_path"].is_string()) {
                    std::filesystem::path p = std::filesystem::path(fallback) / anim_json["frames_path"].get<std::string>();
                    if (p.has_parent_path()) {
                        return p.parent_path().string();
                    }
                }
            }
        }
    } catch (...) {
    }

    return fallback;
}

const nlohmann::json* locate_animation_container(const nlohmann::json& root) {
    if (!root.is_object()) {
        return nullptr;
    }

    auto animations_it = root.find("animations");
    if (animations_it != root.end() && animations_it->is_object()) {
        return &(*animations_it);
    }

    return nullptr;
}

const nlohmann::json* locate_animation_payloads(const nlohmann::json& root) {
    if (!root.is_object()) {
        return nullptr;
    }

    if (const auto* container = locate_animation_container(root)) {
        auto nested = container->find("animations");
        if (nested != container->end() && nested->is_object()) {
            return &(*nested);
        }
        return container;
    }

    return &root;
}

bool extract_start_value(const nlohmann::json& root, std::string& out) {
    if (!root.is_object()) {
        return false;
    }

    if (const auto* container = locate_animation_container(root)) {
        auto start_it = container->find("start");
        if (start_it != container->end() && start_it->is_string()) {
            std::string candidate = start_it->get<std::string>();
            if (!candidate.empty()) {
                out = std::move(candidate);
                return true;
            }
        }
    }

    auto start_it = root.find("start");
    if (start_it != root.end() && start_it->is_string()) {
        std::string candidate = start_it->get<std::string>();
        if (!candidate.empty()) {
            out = std::move(candidate);
            return true;
        }
    }

    if (const auto* payloads = locate_animation_payloads(root)) {
        auto nested_start = payloads->find("start");
        if (nested_start != payloads->end() && nested_start->is_string()) {
            std::string candidate = nested_start->get<std::string>();
            if (!candidate.empty()) {
                out = std::move(candidate);
                return true;
            }
        }
    }

    return false;
}

inline CanvasMetrics canvas_metrics_for(const AssetInfo& info) {
    CanvasMetrics metrics;
    metrics.width = std::max(info.original_canvas_width, 0);
    metrics.height = std::max(info.original_canvas_height, 0);
    return metrics;
}

inline CanvasMetrics metrics_from_json(const nlohmann::json& space) {
    CanvasMetrics metrics;
    metrics.width = std::max(space.value("canvas_width", 0), 0);
    metrics.height = std::max(space.value("canvas_height", 0), 0);
    return metrics;
}

inline float sanitize_scale(float scale) {
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        return 1.0f;
    }
    return scale;
}

inline int compute_scaled_dimension(int dimension, float factor) {
    if (dimension <= 0) return 0;
    double value = static_cast<double>(dimension) * static_cast<double>(factor);
    long long rounded = std::llround(value);
    if (rounded < 0) {
        return 0;
    }
    if (rounded > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(rounded);
}

inline SDL_Point canonical_anchor(const CanvasMetrics& canvas) {
    SDL_Point anchor{0, 0};
    anchor.x = (canvas.width > 0) ? canvas.width / 2 : 0;
    anchor.y = canvas.height;
    return anchor;
}

inline SDL_Point scaled_anchor_point(const CanvasMetrics& canvas, float scale) {
    SDL_Point anchor{0, 0};
    const int scaled_w = compute_scaled_dimension(canvas.width, scale);
    const int scaled_h = compute_scaled_dimension(canvas.height, scale);
    anchor.x = (scaled_w > 0) ? scaled_w / 2 : 0;
    anchor.y = scaled_h;
    return anchor;
}

inline int unscale_dimension(int dimension, float scale) {
    if (!(scale > 0.0f) || !std::isfinite(scale)) {
        return dimension;
    }
    if (dimension <= 0) {
        return 0;
    }
    const double value = static_cast<double>(dimension) / static_cast<double>(scale);
    const long long rounded = std::llround(value);
    if (rounded < 0) {
        return 0;
    }
    if (rounded > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(rounded);
}

inline nlohmann::json encode_canonical_points(const std::vector<Area::Point>& points,
                                              SDL_Point anchor,
                                              float scale) {
    nlohmann::json arr = nlohmann::json::array();
    auto& out = arr.get_ref<nlohmann::json::array_t&>();
    out.reserve(points.size());
    for (const auto& p : points) {
        const long long dx_scaled = static_cast<long long>(p.x) - static_cast<long long>(anchor.x);
        const long long dy_scaled = static_cast<long long>(p.y) - static_cast<long long>(anchor.y);
        const int canonical_x = static_cast<int>(std::llround(static_cast<double>(dx_scaled) / scale));
        const int canonical_y = static_cast<int>(std::llround(static_cast<double>(dy_scaled) / scale));
        out.push_back({ {"x", canonical_x}, {"y", canonical_y} });
    }
    return arr;
}

inline std::vector<Area::Point> decode_canonical_points(const nlohmann::json& points,
                                                        SDL_Point anchor,
                                                        float scale) {
    std::vector<Area::Point> decoded;
    if (!points.is_array()) return decoded;
    decoded.reserve(points.size());
    for (const auto& entry : points) {
        if (!entry.is_object()) continue;
        const int canonical_x = entry.value("x", 0);
        const int canonical_y = entry.value("y", 0);
        const long long scaled_dx = static_cast<long long>(std::llround(static_cast<double>(canonical_x) * scale));
        const long long scaled_dy = static_cast<long long>(std::llround(static_cast<double>(canonical_y) * scale));
        const long long world_x = static_cast<long long>(anchor.x) + scaled_dx;
        const long long world_y = static_cast<long long>(anchor.y) + scaled_dy;
        Area::Point p{};
        if (world_x < static_cast<long long>(std::numeric_limits<int>::min())) {
            p.x = std::numeric_limits<int>::min();
        } else if (world_x > static_cast<long long>(std::numeric_limits<int>::max())) {
            p.x = std::numeric_limits<int>::max();
        } else {
            p.x = static_cast<int>(world_x);
        }
        if (world_y < static_cast<long long>(std::numeric_limits<int>::min())) {
            p.y = std::numeric_limits<int>::min();
        } else if (world_y > static_cast<long long>(std::numeric_limits<int>::max())) {
            p.y = std::numeric_limits<int>::max();
        } else {
            p.y = static_cast<int>(world_y);
        }
        decoded.push_back(p);
    }
    return decoded;
}

}

SDL_Point AssetInfo::AreaCodec::scaled_anchor(const AssetInfo& info,
                                             std::optional<float> scale_override) {
    const float scale = sanitize_scale(scale_override.value_or(info.scale_factor));
    CanvasMetrics canvas = canvas_metrics_for(info);
    return scaled_anchor_point(canvas, scale);
}

nlohmann::json AssetInfo::AreaCodec::encode_entry(
    const AssetInfo& info,
    const Area& area,
    const std::string& final_type,
    const std::string& final_kind,
    std::optional<AssetInfo::NamedArea::RenderFrame> frame) {
    nlohmann::json entry = nlohmann::json::object();
    entry["name"] = area.get_name();
    if (!final_type.empty()) {
        entry["type"] = final_type;
    }
    if (!final_kind.empty()) {
        entry["kind"] = final_kind;
    }
    entry["schema_version"] = 2;

    if (!frame) {
        for (const auto& na : info.areas) {
            if (!na.area) continue;
            if (na.area->get_name() == area.get_name() && na.render_frame) {
                frame = na.render_frame;
                break;
            }
        }
    }

    const float info_scale = sanitize_scale(info.scale_factor);
    const float save_scale = sanitize_scale(frame ? frame->pixel_scale : info_scale);
    CanvasMetrics canonical_canvas = canvas_metrics_for(info);
    nlohmann::json coordinate_space = {
        {"origin", "bottom_center"},
        {"scale_at_save", save_scale}
};

    SDL_Point render_anchor{0, 0};
    if (frame && frame->is_valid()) {
        coordinate_space["kind"] = "render_space";
        coordinate_space["canvas_width"] = frame->width;
        coordinate_space["canvas_height"] = frame->height;
        coordinate_space["pivot"] = {
            {"x", frame->pivot_x},
            {"y", frame->pivot_y}
};

        if (canonical_canvas.width <= 0) {
            canonical_canvas.width = unscale_dimension(frame->width, save_scale);
        }
        if (canonical_canvas.height <= 0) {
            canonical_canvas.height = unscale_dimension(frame->height, save_scale);
        }
        render_anchor.x = frame->pivot_x;
        render_anchor.y = frame->pivot_y;
    } else {
        coordinate_space["kind"] = "canonical";
        coordinate_space["canvas_width"] = canonical_canvas.width;
        coordinate_space["canvas_height"] = canonical_canvas.height;
        render_anchor = scaled_anchor_point(canonical_canvas, save_scale);
    }

    entry["coordinate_space"] = coordinate_space;

    const SDL_Point canonical_anchor_point = canonical_anchor(canonical_canvas);
    entry["anchor"] = { {"x", canonical_anchor_point.x}, {"y", canonical_anchor_point.y} };
    entry["points"] = encode_canonical_points(area.get_points(), render_anchor, save_scale);
    entry["resolution"] = area.resolution();
    return entry;
}

std::optional<AssetInfo::NamedArea>
AssetInfo::AreaCodec::decode_entry(const AssetInfo& info, const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    const std::string name = entry.value("name", std::string{});
    if (name.empty()) {
        return std::nullopt;
    }
    if (!entry.contains("points") || !entry["points"].is_array()) {
        return std::nullopt;
    }
    if (!entry.contains("coordinate_space") || !entry["coordinate_space"].is_object()) {
        return std::nullopt;
    }

    const auto& space = entry["coordinate_space"];
    const std::string origin = space.value("origin", std::string{});
    if (origin != "bottom_center") {
        return std::nullopt;
    }

    const std::string space_kind = space.value("kind", std::string{});
    const float saved_scale = sanitize_scale(space.value("scale_at_save", 1.0f));
    const float current_scale = sanitize_scale(info.scale_factor);

    CanvasMetrics canonical_canvas = canvas_metrics_for(info);
    CanvasMetrics saved_canvas = metrics_from_json(space);

    SDL_Point render_anchor = scaled_anchor_point(canonical_canvas, current_scale);
    std::optional<AssetInfo::NamedArea::RenderFrame> frame;

    if (space_kind == "render_space") {
        AssetInfo::NamedArea::RenderFrame rf;
        rf.width = saved_canvas.width;
        rf.height = saved_canvas.height;
        if (space.contains("pivot") && space["pivot"].is_object()) {
            rf.pivot_x = space["pivot"].value("x", rf.width / 2);
            rf.pivot_y = space["pivot"].value("y", rf.height);
        } else {
            rf.pivot_x = (rf.width > 0) ? rf.width / 2 : 0;
            rf.pivot_y = rf.height;
        }
        rf.pixel_scale = saved_scale;

        if (rf.is_valid()) {
            frame = rf;

            if (canonical_canvas.width <= 0) {
                canonical_canvas.width = unscale_dimension(rf.width, rf.pixel_scale);
            }
            if (canonical_canvas.height <= 0) {
                canonical_canvas.height = unscale_dimension(rf.height, rf.pixel_scale);
            }

            const int scaled_w = compute_scaled_dimension(canonical_canvas.width, current_scale);
            const int scaled_h = compute_scaled_dimension(canonical_canvas.height, current_scale);
            const double ratio_x = (rf.width > 0) ? static_cast<double>(rf.pivot_x) / static_cast<double>(rf.width) : 0.5;
            const double ratio_y = (rf.height > 0) ? static_cast<double>(rf.pivot_y) / static_cast<double>(rf.height) : 1.0;
            render_anchor.x = static_cast<int>(std::llround(ratio_x * static_cast<double>(scaled_w)));
            render_anchor.y = static_cast<int>(std::llround(ratio_y * static_cast<double>(scaled_h)));
        }
    } else if (space_kind == "canonical") {
        if (canonical_canvas.width <= 0) {
            canonical_canvas.width = saved_canvas.width;
        }
        if (canonical_canvas.height <= 0) {
            canonical_canvas.height = saved_canvas.height;
        }
        render_anchor = scaled_anchor_point(canonical_canvas, current_scale);
    } else {
        return std::nullopt;
    }

    std::vector<Area::Point> points = decode_canonical_points(entry["points"], render_anchor, current_scale);
    if (points.size() < 3) {
        return std::nullopt;
    }

    NamedArea named;
    named.name = name;
    named.type = entry.value("type", std::string{});
    named.kind = entry.value("kind", named.type);
    if (named.kind.empty()) {
        named.kind = named.type;
    }

    try {
        if (entry.contains("attachment_subtype") && entry["attachment_subtype"].is_string()) {
            named.attachment_subtype = entry["attachment_subtype"].get<std::string>();
        }
        if (entry.contains("is_on_top") && entry["is_on_top"].is_boolean()) {
            named.attachment_is_on_top = entry["is_on_top"].get<bool>();
        } else if (entry.contains("placed_on_top_parent") && entry["placed_on_top_parent"].is_boolean()) {

            named.attachment_is_on_top = entry["placed_on_top_parent"].get<bool>();
        }
    } catch (...) {

    }
    const int resolution = vibble::grid::clamp_resolution(entry.value("resolution", 2));
    named.area = std::make_unique<Area>(name, points, resolution);
    named.area->set_resolution(resolution);
    const std::string& applied_type = !named.type.empty() ? named.type : named.kind;
    if (!applied_type.empty()) {
        named.area->set_type(applied_type);
    }
    named.render_frame = frame;
    return named;
}
namespace {

AssetInfo::ManifestStoreProvider& manifest_store_provider_slot() {
    static AssetInfo::ManifestStoreProvider provider;
    return provider;
}

}

AssetInfo::AssetInfo(const std::string &asset_folder_name)
    : AssetInfo(asset_folder_name, nlohmann::json::object()) {}

AssetInfo::AssetInfo(const std::string& asset_folder_name, const nlohmann::json& metadata)
{
        nlohmann::json data = metadata.is_object() ? metadata : nlohmann::json::object();

        std::string resolved_name = data.value("asset_name", asset_folder_name);
        if (resolved_name.empty()) {
                resolved_name = asset_folder_name;
        }
        name = resolved_name;

        const std::string default_dir = assets_root_for(resolved_name).generic_string();
        dir_path_ = derive_asset_directory(data, default_dir);
        if (dir_path_.empty()) {
                dir_path_ = default_dir;
        }
        dir_path_ = prefer_assets_directory(dir_path_, resolved_name);
        info_json_path_.clear();

        initialize_from_json(data);

        if (!info_json_.contains("asset_name") || !info_json_["asset_name"].is_string() || info_json_["asset_name"].get<std::string>().empty()) {
                info_json_["asset_name"] = name;
        }
}


std::shared_ptr<AssetInfo> AssetInfo::from_manifest_entry(const std::string& asset_folder_name,
                                                         const nlohmann::json& metadata) {
    nlohmann::json meta = metadata.is_object() ? metadata : nlohmann::json::object();
    const bool has_manifest_payload = meta.is_object() && !meta.empty();
    // Only fall back to bundle metadata if the manifest entry is missing/empty.
    const std::filesystem::path bundle_path = std::filesystem::path("cache") / asset_folder_name / "bundle.bin";
    CacheManager::BundleData bundle;
    if (!has_manifest_payload && CacheManager::load_bundle(bundle_path.generic_string(), bundle)) {
        if (bundle.metadata_snapshot.is_object()) {
            meta = bundle.metadata_snapshot;
        }
    }
return std::make_shared<AssetInfo>(asset_folder_name, meta);
}


void AssetInfo::set_manifest_store_provider(ManifestStoreProvider provider) {
    manifest_store_provider_slot() = std::move(provider);
}

AssetInfo::~AssetInfo() {
	for (auto &[key, anim] : animations) {
                anim.clear_texture_cache();
	}
	animations.clear();
}

void AssetInfo::load_base_properties(const nlohmann::json &data) {
        type = asset_types::canonicalize(data.value("asset_type", std::string{asset_types::object}));
        if (type == asset_types::player) {
                std::cout << "[AssetInfo] Player asset '" << name << "' loaded\n\n";
        }
        start_animation = data.value("start", std::string{"default"});
        passable = has_tag("passable");
        try {
                if (data.contains("tillable")) {
                        tillable = data.at("tillable").get<bool>();
                } else if (data.contains("tileable")) {
                        tillable = data.at("tileable").get<bool>();
                } else if (info_json_.contains("tillable")) {
                        tillable = info_json_.value("tillable", false);
                } else if (info_json_.contains("tileable")) {
                        tillable = info_json_.value("tileable", false);
                } else {
                        tillable = false;
                }
        } catch (...) {

                if (info_json_.contains("tillable")) {
                        tillable = info_json_.value("tillable", false);
                } else {
                        tillable = info_json_.value("tileable", false);
                }
        }
        min_same_type_distance = data.value("min_same_type_distance", 0);
        min_distance_all = data.value("min_distance_all", 0);
        flipable = data.value("can_invert", false);
        info_json_["tillable"] = tillable;
        NeighborSearchRadius = std::clamp( data.value("neighbor_search_distance", NeighborSearchRadius), 20, 1000);
        info_json_["neighbor_search_distance"] = NeighborSearchRadius;
        if (info_json_.is_object()) {
                info_json_.erase("apply_parallax");
        }
        starting_health = data.value("starting_health", starting_health);
        lighting_normal_map = data.value("lighting_normal_map", std::string{});
        lighting_roughness = std::clamp(data.value("lighting_roughness", 1.0f), 0.0f, 1.0f);
        lighting_height_bias = data.value("lighting_height_bias", 0.0f);
        if (!std::isfinite(lighting_height_bias)) lighting_height_bias = 0.0f;
        if (!std::isfinite(lighting_roughness)) lighting_roughness = 1.0f;
}

bool AssetInfo::has_tag(const std::string &tag) const {
    return tag_lookup_.find(tag) != tag_lookup_.end();
}

bool AssetInfo::AnimationTextureRebuildRequest::empty() const {
    return all_frames_variants == kTextureVariantNone && frame_variants.empty();
}

void AssetInfo::AnimationTextureRebuildRequest::clear() {
    all_frames_variants = kTextureVariantNone;
    frame_variants.clear();
}

void AssetInfo::AnimationTextureRebuildRequest::mark_animation(std::uint8_t variants) {
    variants = AssetInfo::sanitize_texture_variant_mask(variants);
    if (variants == kTextureVariantNone) {
        return;
    }
    all_frames_variants = static_cast<std::uint8_t>(all_frames_variants | variants);
    if (all_frames_variants == kTextureVariantAll) {
        frame_variants.clear();
        return;
    }

    for (auto it = frame_variants.begin(); it != frame_variants.end();) {
        it->second = AssetInfo::sanitize_texture_variant_mask(
            static_cast<std::uint8_t>(it->second & static_cast<std::uint8_t>(~all_frames_variants)));
        if (it->second == kTextureVariantNone) {
            it = frame_variants.erase(it);
        } else {
            ++it;
        }
    }
}

void AssetInfo::AnimationTextureRebuildRequest::mark_frame(int frame_index, std::uint8_t variants) {
    if (frame_index < 0) {
        return;
    }
    variants = AssetInfo::sanitize_texture_variant_mask(variants);
    if (variants == kTextureVariantNone) {
        return;
    }
    variants = static_cast<std::uint8_t>(variants & static_cast<std::uint8_t>(~all_frames_variants));
    if (variants == kTextureVariantNone) {
        return;
    }
    auto& frame_mask = frame_variants[frame_index];
    frame_mask = AssetInfo::sanitize_texture_variant_mask(static_cast<std::uint8_t>(frame_mask | variants));
    if (frame_mask == kTextureVariantNone) {
        frame_variants.erase(frame_index);
    }
}

void AssetInfo::AnimationTextureRebuildRequest::merge(const AnimationTextureRebuildRequest& other) {
    mark_animation(other.all_frames_variants);
    for (const auto& entry : other.frame_variants) {
        mark_frame(entry.first, entry.second);
    }
}

bool AssetInfo::TextureRebuildBucket::empty() const {
    if (bundle_refresh_required) {
        return false;
    }
    for (const auto& entry : animations) {
        if (!entry.second.empty()) {
            return false;
        }
    }
    return true;
}

void AssetInfo::TextureRebuildBucket::clear() {
    bundle_refresh_required = false;
    animations.clear();
}

void AssetInfo::TextureRebuildBucket::mark_bundle_refresh() {
    bundle_refresh_required = true;
}

void AssetInfo::TextureRebuildBucket::mark_animation(const std::string& animation_name, std::uint8_t variants) {
    if (animation_name.empty()) {
        return;
    }
    variants = AssetInfo::sanitize_texture_variant_mask(variants);
    if (variants == kTextureVariantNone) {
        return;
    }
    animations[animation_name].mark_animation(variants);
    bundle_refresh_required = true;
}

void AssetInfo::TextureRebuildBucket::mark_frame(const std::string& animation_name, int frame_index, std::uint8_t variants) {
    if (animation_name.empty() || frame_index < 0) {
        return;
    }
    variants = AssetInfo::sanitize_texture_variant_mask(variants);
    if (variants == kTextureVariantNone) {
        return;
    }
    animations[animation_name].mark_frame(frame_index, variants);
    bundle_refresh_required = true;
}

void AssetInfo::TextureRebuildBucket::merge(const TextureRebuildBucket& other) {
    bundle_refresh_required = bundle_refresh_required || other.bundle_refresh_required;
    for (const auto& entry : other.animations) {
        if (entry.first.empty()) {
            continue;
        }
        animations[entry.first].merge(entry.second);
        if (animations[entry.first].empty()) {
            animations.erase(entry.first);
        }
    }
}

void AssetInfo::RuntimeTextureRebuildState::clear() {
    pending_on_load.clear();
    pending_on_close.clear();
}

std::uint8_t AssetInfo::sanitize_texture_variant_mask(std::uint8_t variants) {
    return static_cast<std::uint8_t>(variants & kTextureVariantAll);
}

std::uint8_t AssetInfo::classify_texture_rebuild_variants(const nlohmann::json& before_payload,
                                                          const nlohmann::json& after_payload) {
    const std::string before_kind = source_value_string(before_payload, "kind");
    const std::string before_path = source_value_string(before_payload, "path");
    const std::string before_name = source_value_string(before_payload, "name");
    const std::string after_kind = source_value_string(after_payload, "kind");
    const std::string after_path = source_value_string(after_payload, "path");
    const std::string after_name = source_value_string(after_payload, "name");

    const bool source_changed =
        before_kind != after_kind || before_path != after_path || before_name != after_name;
    if (source_changed) {
        return kTextureVariantAll;
    }

    auto read_frame_count = [](const nlohmann::json& payload) -> int {
        if (!payload.is_object() || !payload.contains("number_of_frames")) {
            return -1;
        }
        const auto& value = payload["number_of_frames"];
        try {
            if (value.is_number_integer()) {
                return value.get<int>();
            }
            if (value.is_number_float()) {
                return static_cast<int>(std::lround(value.get<double>()));
            }
            if (value.is_string()) {
                return std::stoi(value.get<std::string>());
            }
        } catch (...) {
            return -1;
        }
        return -1;
    };

    const int before_frames = read_frame_count(before_payload);
    const int after_frames = read_frame_count(after_payload);
    const bool has_source_hint =
        !before_kind.empty() || !after_kind.empty() || !before_path.empty() || !after_path.empty();
    if (has_source_hint && before_frames != after_frames) {
        return kTextureVariantAll;
    }

    return kTextureVariantNone;
}

void AssetInfo::clear_runtime_texture_rebuild_state() {
    runtime_texture_rebuild_state_.clear();
}

void AssetInfo::mark_texture_rebuild_on_close(const std::string& animation_name, std::uint8_t variants) {
    runtime_texture_rebuild_state_.pending_on_close.mark_animation(animation_name, variants);
}

void AssetInfo::mark_texture_frame_rebuild_on_close(const std::string& animation_name,
                                                    int frame_index,
                                                    std::uint8_t variants) {
    runtime_texture_rebuild_state_.pending_on_close.mark_frame(animation_name, frame_index, variants);
}

void AssetInfo::mark_all_animation_textures_on_close(std::uint8_t variants) {
    variants = sanitize_texture_variant_mask(variants);
    if (variants == kTextureVariantNone) {
        return;
    }

    bool marked_any = false;
    if (anims_json_.is_object()) {
        for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
            if (!it.value().is_object() || it.key().empty()) {
                continue;
            }
            runtime_texture_rebuild_state_.pending_on_close.mark_animation(it.key(), variants);
            marked_any = true;
        }
    }

    if (!marked_any) {
        if (const auto* payloads = locate_animation_payloads(info_json_)) {
            if (payloads->is_object()) {
                for (auto it = payloads->begin(); it != payloads->end(); ++it) {
                    if (!it.value().is_object() || it.key().empty()) {
                        continue;
                    }
                    runtime_texture_rebuild_state_.pending_on_close.mark_animation(it.key(), variants);
                    marked_any = true;
                }
            }
        }
    }

    if (!marked_any) {
        runtime_texture_rebuild_state_.pending_on_close.mark_bundle_refresh();
    }
}

void AssetInfo::mark_bundle_refresh_on_close() {
    runtime_texture_rebuild_state_.pending_on_close.mark_bundle_refresh();
}

void AssetInfo::mark_texture_rebuild_on_load(const std::string& animation_name, std::uint8_t variants) {
    runtime_texture_rebuild_state_.pending_on_load.mark_animation(animation_name, variants);
}

void AssetInfo::mark_texture_frame_rebuild_on_load(const std::string& animation_name,
                                                   int frame_index,
                                                   std::uint8_t variants) {
    runtime_texture_rebuild_state_.pending_on_load.mark_frame(animation_name, frame_index, variants);
}

AssetInfo::TextureRebuildBucket AssetInfo::consume_pending_texture_rebuild_on_close() {
    TextureRebuildBucket pending = runtime_texture_rebuild_state_.pending_on_close;
    runtime_texture_rebuild_state_.pending_on_close.clear();
    return pending;
}

AssetInfo::TextureRebuildBucket AssetInfo::consume_pending_texture_rebuild_on_load() {
    TextureRebuildBucket pending = runtime_texture_rebuild_state_.pending_on_load;
    runtime_texture_rebuild_state_.pending_on_load.clear();
    return pending;
}

void AssetInfo::merge_pending_texture_rebuild_on_close(const TextureRebuildBucket& pending) {
    runtime_texture_rebuild_state_.pending_on_close.merge(pending);
}

void AssetInfo::merge_pending_texture_rebuild_on_load(const TextureRebuildBucket& pending) {
    runtime_texture_rebuild_state_.pending_on_load.merge(pending);
}

void AssetInfo::classify_animation_snapshot_rebuilds(const nlohmann::json& before_snapshot,
                                                     const nlohmann::json& after_snapshot) {
    const nlohmann::json* before_anims = snapshot_animations_object(before_snapshot);
    const nlohmann::json* after_anims = snapshot_animations_object(after_snapshot);
    if ((!before_anims || !before_anims->is_object()) && (!after_anims || !after_anims->is_object())) {
        return;
    }

    std::unordered_set<std::string> animation_names;
    if (before_anims && before_anims->is_object()) {
        for (auto it = before_anims->begin(); it != before_anims->end(); ++it) {
            animation_names.insert(it.key());
        }
    }
    if (after_anims && after_anims->is_object()) {
        for (auto it = after_anims->begin(); it != after_anims->end(); ++it) {
            animation_names.insert(it.key());
        }
    }

    for (const auto& animation_name : animation_names) {
        const bool in_before = before_anims && before_anims->is_object() &&
                               before_anims->contains(animation_name) &&
                               (*before_anims)[animation_name].is_object();
        const bool in_after = after_anims && after_anims->is_object() &&
                              after_anims->contains(animation_name) &&
                              (*after_anims)[animation_name].is_object();

        if (!in_before && in_after) {
            mark_texture_rebuild_on_close(animation_name, kTextureVariantAll);
            continue;
        }
        if (in_before && !in_after) {
            mark_bundle_refresh_on_close();
            continue;
        }
        if (!in_before || !in_after) {
            continue;
        }

        const auto variants = classify_texture_rebuild_variants((*before_anims)[animation_name],
                                                                (*after_anims)[animation_name]);
        if (variants != kTextureVariantNone) {
            mark_texture_rebuild_on_close(animation_name, variants);
        }
    }
}

nlohmann::json AssetInfo::manifest_payload() const {
        nlohmann::json payload = info_json_;
        if (!payload.is_object()) {
                payload = nlohmann::json::object();
        }
        payload[kMovementEnabledKey] = movement_enabled;
        payload[kAttackBoxEnabledKey] = attack_box_enabled;
        payload[kHitboxEnabledKey] = hitbox_enabled;
        payload[kImpassableEnabledKey] = impassable_enabled;
        payload[kFloorBoxesEnabledKey] = floor_boxes_enabled;
        payload.erase("impassable_box_enabled");
        payload.erase("impassable_boxes");
        if (floor_boxes_enabled) {
                const nlohmann::json floor_payload = encode_floor_boxes_payload(floor_boxes);
                if (!floor_payload.empty()) {
                        payload[kFloorBoxesKey] = floor_payload;
                } else {
                        payload.erase(kFloorBoxesKey);
                }
        } else {
                payload.erase(kFloorBoxesKey);
        }
        if (impassable_enabled) {
                const nlohmann::json impassable_payload = encode_impassable_shapes_payload(impassable_shapes);
                if (!impassable_payload.empty()) {
                        payload[kImpassableShapesKey] = impassable_payload;
                } else {
                        payload.erase(kImpassableShapesKey);
                }
        } else {
        payload.erase(kImpassableShapesKey);
        }
        payload["weight_kg"] = weight_kg;
        payload["bounce_amount"] = bounce_amount;
        if (!payload.contains("size_settings") || !payload["size_settings"].is_object()) {
                payload["size_settings"] = nlohmann::json::object();
        }
        payload["size_settings"]["size_variation"] = size_variation_percent;
        payload[kTiltRangeKey] = vibble::weighted_range::to_json(tilt_range);
        payload[kYPositionRangeKey] = vibble::weighted_range::to_json(y_position_range);
        payload["tilt_range_min_deg"] = tilt_range_min_deg;
        payload["tilt_range_max_deg"] = tilt_range_max_deg;
        payload.erase("y_pos_min");
        payload.erase("y_pos_max");
        payload[kAnchorPointChildCandidatesKey] = anchor_point_child_candidates_payload();
        payload.erase(kAnchorPointChildCandidatesLegacyKey);
        payload[kOvalAnchorMappingsKey] = oval_anchor_mappings_payload();
        payload["lighting_normal_map"] = lighting_normal_map;
        payload["lighting_roughness"] = std::clamp(lighting_roughness, 0.0f, 1.0f);
        payload["lighting_height_bias"] = std::isfinite(lighting_height_bias) ? lighting_height_bias : 0.0f;
        if (payload.contains("animations") && payload["animations"].is_object()) {
                for (auto it = payload["animations"].begin(); it != payload["animations"].end(); ++it) {
                        if (!it.value().is_object()) {
                                continue;
                        }
                        apply_system_enable_flags_to_animation_payload(it.value(),
                                                                       movement_enabled,
                                                                       hitbox_enabled,
                                                                       attack_box_enabled);
                }
        }
        if (!payload.contains("asset_name") || !payload["asset_name"].is_string() || payload["asset_name"].get<std::string>().empty()) {
                payload["asset_name"] = name;
        }
        return payload;
}

void AssetInfo::mark_dirty() {
        dirty_ = true;
}

bool AssetInfo::is_dirty() const {
        return dirty_;
}

bool AssetInfo::save_self_to_manifest(devmode::core::ManifestStore* store) {
        nlohmann::json payload = manifest_payload();

        devmode::core::ManifestStore* target_store = store;
        if (!target_store) {
                auto& provider = manifest_store_provider_slot();
                if (provider) {
                        target_store = provider();
                }
        }
        if (!target_store) {
                std::cerr << "[AssetInfo] Manifest store unavailable for '" << name << "'; cannot persist asset manifest entry.\n";
                return false;
        }

        try {
                auto guard = target_store->scoped_guard("AssetInfo::save_self_to_manifest");
                auto txn = target_store->begin_asset_transaction(name, true);
                if (!txn) {
                        std::cerr << "[AssetInfo] Failed to begin manifest transaction for '" << name << "'.\n";
                        return false;
                }
                txn.data() = payload;
                if (!txn.finalize()) {
                        std::cerr << "[AssetInfo] Failed to finalize manifest transaction for '" << name << "'.\n";
                        return false;
                }
                target_store->flush();
        } catch (const std::exception& ex) {
                std::cerr << "[AssetInfo] Failed to persist manifest entry for '" << name << "': " << ex.what() << "\n";
                return false;
        } catch (...) {
                std::cerr << "[AssetInfo] Unknown error persisting manifest entry for '" << name << "'\n";
                return false;
        }

        info_json_ = std::move(payload);
        return true;
}

bool AssetInfo::save_self_to_cache_if_dirty(SDL_Renderer* renderer) {
        (void)renderer;
        if (!dirty_) {
                return true;
        }

        // Cache regeneration is runtime-flag driven (pending_on_close / pending_on_load).
        // Dirty here tracks manifest/session mutation only.
        dirty_ = false;
        return true;
}

bool AssetInfo::commit_manifest() {
        const bool manifest_saved = save_self_to_manifest();
        if (!manifest_saved) {
                return false;
        }
        mark_dirty();
        return true;
}

void AssetInfo::set_asset_type(const std::string &t) {
        std::string canonical = asset_types::canonicalize(t);
        type = canonical;
        info_json_["asset_type"] = canonical;
}

void AssetInfo::set_min_same_type_distance(int d) {
	min_same_type_distance = d;
	info_json_["min_same_type_distance"] = d;
}

void AssetInfo::set_min_distance_all(int d) {
        min_distance_all = d;
        info_json_["min_distance_all"] = d;
}

void AssetInfo::set_neighbor_search_radius(int radius) {
        NeighborSearchRadius = std::clamp(radius, 20, 1000);
        info_json_["neighbor_search_distance"] = NeighborSearchRadius;
}

void AssetInfo::set_flipable(bool v) {
        flipable = v;
        info_json_["can_invert"] = v;
}

void AssetInfo::set_starting_health(int health) {
        starting_health = health;
        info_json_["starting_health"] = health;
}

void AssetInfo::set_scale_factor(float factor) {
	if (factor < 0.f)
	factor = 0.f;
	scale_factor = factor;
	if (!info_json_.contains("size_settings") ||
	!info_json_["size_settings"].is_object()) {
		info_json_["size_settings"] = nlohmann::json::object();
	}
	info_json_["size_settings"]["scale_percentage"] = factor * 100.0f;
}

void AssetInfo::set_scale_percentage(float percent) {
        scale_factor = percent / 100.0f;
        if (!info_json_.contains("size_settings") ||
        !info_json_["size_settings"].is_object()) {
                info_json_["size_settings"] = nlohmann::json::object();
        }
        info_json_["size_settings"]["scale_percentage"] = percent;
}

void AssetInfo::set_weight_kg(float weight) {
        if (weight < 0.0f) {
                weight = 0.0f;
        }
        weight_kg = weight;
        info_json_["weight_kg"] = weight;
}

void AssetInfo::set_bounce_amount(int amount) {
        bounce_amount = std::clamp(amount, 0, 100);
        info_json_["bounce_amount"] = bounce_amount;
}

void AssetInfo::set_size_variation_range(const vibble::weighted_range::WeightedIntRange& range) {
        size_variation_range = vibble::weighted_range::from_json(vibble::weighted_range::to_json(range), vibble::weighted_range::make_flat(0));
        size_variation_percent = clamp_size_variation_percent(static_cast<float>(size_variation_range.center));
        if (!info_json_.contains("size_settings") ||
        !info_json_["size_settings"].is_object()) {
                info_json_["size_settings"] = nlohmann::json::object();
        }
        info_json_["size_settings"]["size_variation"] = size_variation_percent;
}

void AssetInfo::set_size_variation_percentage(float percent) {
        set_size_variation_range(vibble::weighted_range::make_flat(static_cast<std::int64_t>(std::llround(clamp_size_variation_percent(percent)))));
}

void AssetInfo::set_tilt_range(const vibble::weighted_range::WeightedIntRange& range) {
        tilt_range = vibble::weighted_range::from_json(vibble::weighted_range::to_json(range), vibble::weighted_range::make_flat(0));
        std::tie(tilt_range_min_deg, tilt_range_max_deg) = tilt_range_bounds(tilt_range);
        info_json_[kTiltRangeKey] = vibble::weighted_range::to_json(tilt_range);
        info_json_["tilt_range_min_deg"] = tilt_range_min_deg;
        info_json_["tilt_range_max_deg"] = tilt_range_max_deg;
}

void AssetInfo::set_tilt_range_degrees(int min_deg, int max_deg) {
        min_deg = clamp_tilt_degrees(min_deg);
        max_deg = clamp_tilt_degrees(max_deg);
        if (min_deg > max_deg) {
                std::swap(min_deg, max_deg);
        }
        set_tilt_range(vibble::weighted_range::make_legacy_uniform(min_deg, max_deg));
}

void AssetInfo::set_y_position_range(const vibble::weighted_range::WeightedIntRange& range) {
        y_position_range = sanitize_y_position_range_percent(
            vibble::weighted_range::from_json(vibble::weighted_range::to_json(range),
                                              vibble::weighted_range::make_flat(0)));
        info_json_[kYPositionRangeKey] = vibble::weighted_range::to_json(y_position_range);
        info_json_.erase("y_pos_min");
        info_json_.erase("y_pos_max");
}

void AssetInfo::set_scale_filter(bool smooth) {
        smooth_scaling = smooth;
        if (!info_json_.contains("size_settings") ||
        !info_json_["size_settings"].is_object()) {
                info_json_["size_settings"] = nlohmann::json::object();
        }
        info_json_["size_settings"]["scale_filter"] = smooth ? "linear" : "nearest";
}



void AssetInfo::set_tags(const std::vector<std::string> &t) {
        tags = t;
        rebuild_tag_cache();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &s : tags)
        arr.push_back(s);
        info_json_["tags"] = std::move(arr);
        passable = has_tag("passable");
}

void AssetInfo::add_tag(const std::string &tag) {
        if (!has_tag(tag)) {
                tags.push_back(tag);
        }
        set_tags(tags);
}

void AssetInfo::remove_tag(const std::string &tag) {
        tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
        set_tags(tags);
}

void AssetInfo::set_anti_tags(const std::vector<std::string> &t) {
        anti_tags = t;
        rebuild_anti_tag_cache();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &s : anti_tags)
                arr.push_back(s);
        info_json_["anti_tags"] = std::move(arr);
}

void AssetInfo::add_anti_tag(const std::string &tag) {
        if (anti_tag_lookup_.find(tag) == anti_tag_lookup_.end()) {
                anti_tags.push_back(tag);
        }
        set_anti_tags(anti_tags);
}

void AssetInfo::remove_anti_tag(const std::string &tag) {
        anti_tags.erase(std::remove(anti_tags.begin(), anti_tags.end(), tag), anti_tags.end());
        set_anti_tags(anti_tags);
}

void AssetInfo::rebuild_tag_cache() {
        tag_lookup_.clear();
        tag_lookup_.reserve(tags.size());
        for (const auto& value : tags) {
                tag_lookup_.insert(value);
        }
}

void AssetInfo::rebuild_anti_tag_cache() {
    anti_tag_lookup_.clear();
    anti_tag_lookup_.reserve(anti_tags.size());
    for (const auto& value : anti_tags) {
        anti_tag_lookup_.insert(value);
    }
}

#if defined(ASSET_INFO_ENABLE_TEST_ACCESS)
void AssetInfoTestAccess::initialize_info_json(AssetInfo& info, nlohmann::json data) {
    info.info_json_ = std::move(data);
}

void AssetInfoTestAccess::rebuild_tag_cache(AssetInfo& info) {
    info.rebuild_tag_cache();
}

void AssetInfoTestAccess::rebuild_anti_tag_cache(AssetInfo& info) {
    info.rebuild_anti_tag_cache();
}
#endif

void AssetInfo::set_passable(bool v) {
        passable = v;
        if (v)
        add_tag("passable");
        else
        remove_tag("passable");
}

void AssetInfo::set_tillable(bool v) {
        tillable = v;

        info_json_["tillable"] = v;
        info_json_["tileable"] = v;
}

Area* AssetInfo::find_area(const std::string& name) {
	for (auto& na : areas) {
		if (na.name == name) return na.area.get();
	}
	return nullptr;
}
void AssetInfo::upsert_area_from_editor(const Area& area,
                                        std::optional<NamedArea::RenderFrame> frame) {
    if (area.get_name().empty()) {
        return;
    }

    if (!info_json_.contains("areas") || !info_json_["areas"].is_array()) {
        info_json_["areas"] = nlohmann::json::array();
    }

    nlohmann::json* existing_entry = nullptr;
    std::string existing_type;
    std::string existing_kind;
    for (auto& entry : info_json_["areas"]) {
        if (!entry.is_object()) continue;
        if (entry.value("name", std::string{}) == area.get_name()) {
            existing_entry = &entry;
            existing_type = entry.value("type", std::string{});
            existing_kind = entry.value("kind", std::string{});
            break;
        }
    }

    const std::string final_type = !area.get_type().empty() ? area.get_type() : existing_type;
    std::string final_kind = existing_kind;
    if (final_kind.empty()) final_kind = final_type;

    bool updated = false;
    for (auto& na : areas) {
        if (na.name == area.get_name()) {
            na.area = std::make_unique<Area>(area);
            if (!final_type.empty()) na.type = final_type;
            if (!final_kind.empty()) na.kind = final_kind;
            na.render_frame = frame;
            updated = true;
            break;
        }
    }
    if (!updated) {
        NamedArea na;
        na.name = area.get_name();
        na.type = final_type;
        na.kind = final_kind;
        na.area = std::make_unique<Area>(area);
        na.render_frame = frame;
        areas.push_back(std::move(na));
    }

    nlohmann::json entry =
        AreaCodec::encode_entry(*this, area, final_type, final_kind, frame);

    if (existing_entry && existing_entry->is_object()) {
        static const char* kAttachmentKeys[] = {
            "attachment_subtype", "is_on_top", "placed_on_top_parent"
};
        for (const char* key : kAttachmentKeys) {
            auto it = existing_entry->find(key);
            if (it != existing_entry->end()) {
                entry[key] = *it;
            }
        }
    }

    if (existing_entry) {
        *existing_entry = std::move(entry);
    } else {
        info_json_["areas"].push_back(std::move(entry));
    }
}

std::string AssetInfo::pick_next_animation(const std::string& mapping_id) const {
	auto it = mappings.find(mapping_id);
	if (it == mappings.end()) return {};
	static std::mt19937 rng{std::random_device{}()};
	for (const auto& entry : it->second) {
		if (!entry.condition.empty() && entry.condition != "true") continue;
		float total = 0.0f;
		for (const auto& opt : entry.options) {
			total += opt.percent;
		}
		if (total <= 0.0f) continue;
		std::uniform_real_distribution<float> dist(0.0f, total);
		float r = dist(rng);
		for (const auto& opt : entry.options) {
			if ((r -= opt.percent) <= 0.0f) {
					return opt.animation;
			}
		}
	}
	return {};
}

void AssetInfo::load_areas(const nlohmann::json& data) {
        areas.clear();
        if (!data.contains("areas") || !data["areas"].is_array()) {
                return;
        }

        for (const auto& entry : data["areas"]) {
                auto decoded = AreaCodec::decode_entry(*this, entry);
                if (!decoded) {
                        continue;
                }
                areas.push_back(std::move(*decoded));
        }
}

void AssetInfo::load_animations(const nlohmann::json& data) {
    const nlohmann::json* payloads = locate_animation_payloads(data);

    nlohmann::json new_anim = nlohmann::json::object();
    if (payloads && payloads->is_object()) {
        for (auto it = payloads->begin(); it != payloads->end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            const auto& anim_json = it.value();
            nlohmann::json converted = anim_json;
            if (!anim_json.contains("source")) {
                converted["source"] = {
                    {"kind", "folder"},
                    {"path", anim_json.value("frames_path", it.key())}
};
                converted["locked"] = anim_json.value("lock_until_done", false);
                converted.erase("frames_path");
                converted.erase("lock_until_done");
                converted.erase("speed");
                converted.erase("speed_factor");
                converted.erase("speed_multiplier");
                converted.erase("fps");
            }
            nlohmann::json normalized = normalize_animation_payload(std::move(converted));
            apply_system_enable_flags_to_animation_payload(normalized,
                                                           movement_enabled,
                                                           hitbox_enabled,
                                                           attack_box_enabled);
            new_anim[it.key()] = std::move(normalized);
        }
    }

    anims_json_ = std::move(new_anim);
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }
    info_json_["animations"] = anims_json_;
}

void AssetInfo::initialize_from_json(const nlohmann::json& source) {
        nlohmann::json data = source.is_object() ? source : nlohmann::json::object();

        info_json_ = data;

        tags = parse_string_array(data.value("tags", nlohmann::json::array()));
        anti_tags = parse_string_array(data.value("anti_tags", nlohmann::json::array()));
        rebuild_tag_cache();
        rebuild_anti_tag_cache();

        if (!info_json_.contains("tags") || !info_json_["tags"].is_array()) {
                info_json_["tags"] = nlohmann::json::array();
        }
        if (!info_json_.contains("anti_tags") || !info_json_["anti_tags"].is_array()) {
                info_json_["anti_tags"] = nlohmann::json::array();
        }
        movement_enabled = data.contains(kMovementEnabledKey) && data[kMovementEnabledKey].is_boolean()
            ? data[kMovementEnabledKey].get<bool>()
            : false;
        attack_box_enabled = data.contains(kAttackBoxEnabledKey) && data[kAttackBoxEnabledKey].is_boolean()
            ? data[kAttackBoxEnabledKey].get<bool>()
            : false;
        hitbox_enabled = data.contains(kHitboxEnabledKey) && data[kHitboxEnabledKey].is_boolean()
            ? data[kHitboxEnabledKey].get<bool>()
            : false;
        impassable_enabled = data.contains(kImpassableEnabledKey) && data[kImpassableEnabledKey].is_boolean()
            ? data[kImpassableEnabledKey].get<bool>()
            : false;
        floor_boxes_enabled = data.contains(kFloorBoxesEnabledKey) && data[kFloorBoxesEnabledKey].is_boolean()
            ? data[kFloorBoxesEnabledKey].get<bool>()
            : false;
        info_json_[kMovementEnabledKey] = movement_enabled;
        info_json_[kAttackBoxEnabledKey] = attack_box_enabled;
        info_json_[kHitboxEnabledKey] = hitbox_enabled;
        info_json_[kImpassableEnabledKey] = impassable_enabled;
        info_json_[kFloorBoxesEnabledKey] = floor_boxes_enabled;
        info_json_.erase("impassable_box_enabled");
        info_json_.erase("impassable_boxes");

        floor_boxes.clear();
        if (floor_boxes_enabled && data.contains(kFloorBoxesKey) && data[kFloorBoxesKey].is_array()) {
                floor_boxes = parse_floor_boxes_payload(data[kFloorBoxesKey]);
                const nlohmann::json floor_payload = encode_floor_boxes_payload(floor_boxes);
                if (!floor_payload.empty()) {
                        info_json_[kFloorBoxesKey] = floor_payload;
                } else {
                        info_json_.erase(kFloorBoxesKey);
                }
        } else {
                info_json_.erase(kFloorBoxesKey);
        }
        impassable_shapes.clear();
        if (impassable_enabled && data.contains(kImpassableShapesKey) && data[kImpassableShapesKey].is_array()) {
                impassable_shapes = parse_impassable_shapes_payload(data[kImpassableShapesKey]);
                const nlohmann::json impassable_payload = encode_impassable_shapes_payload(impassable_shapes);
                if (!impassable_payload.empty()) {
                        info_json_[kImpassableShapesKey] = impassable_payload;
                } else {
                        info_json_.erase(kImpassableShapesKey);
                }
        } else {
                info_json_.erase(kImpassableShapesKey);
        }
        if (data.contains(kAnchorPointChildCandidatesKey)) {
                set_anchor_point_child_candidates_payload(data[kAnchorPointChildCandidatesKey]);
        } else if (data.contains(kAnchorPointChildCandidatesLegacyKey)) {
                set_anchor_point_child_candidates_payload(data[kAnchorPointChildCandidatesLegacyKey]);
        } else {
                set_anchor_point_child_candidates_payload(nlohmann::json::array());
        }
        if (data.contains(kOvalAnchorMappingsKey)) {
                set_oval_anchor_mappings_payload(data[kOvalAnchorMappingsKey]);
        } else {
                set_oval_anchor_mappings_payload(nlohmann::json::array());
        }
        load_animations(data);

        mappings.clear();
        if (data.contains("mappings") && data["mappings"].is_object()) {
                for (auto it = data["mappings"].begin(); it != data["mappings"].end(); ++it) {
                        const std::string id = it.key();
                        Mapping map;
                        if (it.value().is_array()) {
                                for (const auto& entry_json : it.value()) {
                                        if (!entry_json.is_object()) {
                                                continue;
                                        }
                                        MappingEntry me;
                                        me.condition = entry_json.value("condition", "");
                                        if (entry_json.contains("map_to") && entry_json["map_to"].contains("options")) {
                                                for (const auto& opt_json : entry_json["map_to"]["options"]) {
                                                        if (!opt_json.is_object()) {
                                                                continue;
                                                        }
                                                        MappingOption opt{opt_json.value("animation", ""), opt_json.value("percent", 100.0f)};
                                                        me.options.push_back(opt);
                                                }
                                        }
                                        map.push_back(std::move(me));
                                }
                        }
                        mappings[id] = std::move(map);
                }
                info_json_["mappings"] = data["mappings"];
        }

        smooth_scaling = true;
        if (has_tag("pixel_art") || has_tag("preserve_pixels")) {
                smooth_scaling = false;
        }

        load_base_properties(data);

        const auto &ss = data.value("size_settings", nlohmann::json::object());
        scale_factor = ss.value("scale_percentage", 100.0f) / 100.0f;
        if (ss.contains("size_variation") && ss["size_variation"].is_object()) {
                size_variation_range = vibble::weighted_range::from_json(ss["size_variation"],
                                                                         vibble::weighted_range::make_flat(0));
        } else if (ss.contains("size_variation")) {
                const auto value = parse_number_like_json(ss["size_variation"]).value_or(0.0);
                size_variation_range = vibble::weighted_range::make_flat(
                    static_cast<std::int64_t>(std::llround(value)));
        } else {
                size_variation_range = vibble::weighted_range::make_flat(0);
        }
        size_variation_percent = clamp_size_variation_percent(static_cast<float>(size_variation_range.center));
        if (ss.contains("scale_filter")) {
                std::string filter = ss.value("scale_filter", std::string{});
                for (char& ch : filter) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (!filter.empty()) {
                        smooth_scaling = !(filter == "nearest" || filter == "point" || filter == "none");
                }
        }

        try {
                if (data.contains("canvas_width") && data["canvas_width"].is_number_integer()) {
                        original_canvas_width = std::max(0, data["canvas_width"].get<int>());
                }
                if (data.contains("canvas_height") && data["canvas_height"].is_number_integer()) {
                        original_canvas_height = std::max(0, data["canvas_height"].get<int>());
                }
                if (data.contains("weight_kg") && data["weight_kg"].is_number()) {
                        weight_kg = std::max(0.0f, static_cast<float>(data["weight_kg"].get<double>()));
                }
                if (data.contains("bounce_amount") && data["bounce_amount"].is_number()) {
                        bounce_amount = std::clamp(static_cast<int>(std::lround(data["bounce_amount"].get<double>())),
                                                   0,
                                                   100);
                } else {
                        bounce_amount = 0;
                }
                tilt_range = read_weighted_range_field(data, kTiltRangeKey);
                if (!tilt_range.random && (!data.contains(kTiltRangeKey) || !data[kTiltRangeKey].is_object())) {
                        tilt_range = read_weighted_range_legacy_pair(data,
                                                                     "tilt_range_min_deg",
                                                                     "tilt_range_max_deg");
                }
                std::tie(tilt_range_min_deg, tilt_range_max_deg) = tilt_range_bounds(tilt_range);
                y_position_range = sanitize_y_position_range_percent(
                    read_weighted_range_field(data, kYPositionRangeKey, vibble::weighted_range::make_flat(0)));
        } catch (...) {

        }
        info_json_["bounce_amount"] = bounce_amount;
        if (!info_json_.contains("size_settings") || !info_json_["size_settings"].is_object()) {
                info_json_["size_settings"] = nlohmann::json::object();
        }
        info_json_["size_settings"]["size_variation"] = size_variation_percent;
        info_json_[kTiltRangeKey] = vibble::weighted_range::to_json(tilt_range);
        info_json_[kYPositionRangeKey] = vibble::weighted_range::to_json(y_position_range);
        info_json_["tilt_range_min_deg"] = tilt_range_min_deg;
        info_json_["tilt_range_max_deg"] = tilt_range_max_deg;

        if (is_vibble_asset_name(name) && oval_anchor_mappings.empty()) {
                const std::vector<std::string> vibble_mapping_names{
                    "eyes",
                    "hat",
                    "mouth",
                    "neck",
                    "melee",
                };
                std::vector<OvalAnchorMapping> defaults;
                defaults.reserve(vibble_mapping_names.size());
                for (const auto& mapping_name : vibble_mapping_names) {
                        OvalAnchorMapping mapping{};
                        mapping.name = mapping_name;
                        mapping.asset_name = name;
                        mapping.width_radius_x = kDefaultOvalWidthRadius;
                        mapping.height_radius_z = kDefaultOvalHeightRadius;
                        mapping.points = make_default_oval_anchor_points(0,
                                                                          0,
                                                                          mapping.width_radius_x,
                                                                          mapping.height_radius_z,
                                                                          kDefaultOvalPointCount);
                        defaults.push_back(std::move(mapping));
                }
                set_oval_anchor_mappings_payload(build_oval_anchor_mappings_payload(defaults));
        }

        ensure_oval_center_anchors_for_all_mappings();

}


void AssetInfo::set_spawn_groups_payload(const nlohmann::json& groups) {
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }

    if (groups.is_array()) {
        nlohmann::json sanitized = groups;
        for (auto& entry : sanitized) {
            vibble::spawn_group_codec::EntryDefaults defaults{};
            defaults.display_name = vibble::spawn_group_codec::read_string_field(entry, "display_name", "New Spawn");
            vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
        }
        info_json_["spawn_groups"] = std::move(sanitized);
    } else {
        info_json_.erase("spawn_groups");
    }
}

nlohmann::json AssetInfo::spawn_groups_payload() const {
    if (info_json_.is_object()) {
        auto it = info_json_.find("spawn_groups");
        if (it != info_json_.end() && it->is_array()) {
            return *it;
        }
    }
    return nlohmann::json::array();
}

void AssetInfo::set_spawn_groups(const nlohmann::json& groups) {
    nlohmann::json sanitized = nlohmann::json::array();
    if (groups.is_array()) {
        sanitized = groups;
        for (auto& entry : sanitized) {
            vibble::spawn_group_codec::EntryDefaults defaults{};
            defaults.display_name = vibble::spawn_group_codec::read_string_field(entry, "display_name", "New Spawn");
            vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
        }
    }

    info_json_["spawn_groups"] = std::move(sanitized);
}

void AssetInfo::sync_anchor_point_child_candidates_info_json() {
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }
    info_json_[kAnchorPointChildCandidatesKey] = anchor_point_child_candidates_payload();
    info_json_.erase(kAnchorPointChildCandidatesLegacyKey);
}

void AssetInfo::sync_oval_anchor_mappings_info_json() {
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }
    info_json_[kOvalAnchorMappingsKey] = oval_anchor_mappings_payload();
}

void AssetInfo::set_anchor_point_child_candidates_payload(const nlohmann::json& candidates) {
    anchor_point_child_candidates = parse_anchor_point_child_candidates_payload(candidates);
    sync_anchor_point_child_candidates_info_json();
}

nlohmann::json AssetInfo::anchor_point_child_candidates_payload() const {
    return build_anchor_point_child_candidates_payload(anchor_point_child_candidates);
}

void AssetInfo::set_oval_anchor_mappings_payload(const nlohmann::json& mappings) {
    oval_anchor_mappings = parse_oval_anchor_mappings_payload(mappings, name);
    for (auto& mapping : oval_anchor_mappings) {
        mapping.width_radius_x = sanitize_oval_radius(mapping.width_radius_x, kDefaultOvalWidthRadius);
        mapping.height_radius_z = sanitize_oval_radius(mapping.height_radius_z, kDefaultOvalHeightRadius);
        mapping.radius_offset_degrees = sanitize_oval_radius_offset_degrees(mapping.radius_offset_degrees);
        for (auto& point : mapping.points) {
            point.angle_degrees = normalize_angle_degrees(point.angle_degrees);
            recompute_oval_point_position(point, mapping.width_radius_x, mapping.height_radius_z);
            point.tags = normalize_tag_list(point.tags);
            point.anti_tags = normalize_tag_list(point.anti_tags);
        }
    }
    ensure_oval_center_anchors_for_all_mappings();
    sync_oval_anchor_mappings_info_json();
}

nlohmann::json AssetInfo::oval_anchor_mappings_payload() const {
    return build_oval_anchor_mappings_payload(oval_anchor_mappings);
}

bool AssetInfo::upsert_oval_anchor_mapping(const OvalAnchorMapping& mapping) {
    if (!mapping.valid()) {
        return false;
    }

    const nlohmann::json before_payload = oval_anchor_mappings_payload();
    std::vector<OvalAnchorMapping> updated = parse_oval_anchor_mappings_payload(before_payload, name);

    OvalAnchorMapping normalized = mapping;
    normalized.name = trim_copy(normalized.name);
    normalized.asset_name = trim_copy(normalized.asset_name);
    if (normalized.asset_name.empty()) {
        normalized.asset_name = name;
    }
    normalized.width_radius_x = sanitize_oval_radius(normalized.width_radius_x, kDefaultOvalWidthRadius);
    normalized.height_radius_z = sanitize_oval_radius(normalized.height_radius_z, kDefaultOvalHeightRadius);
    normalized.radius_offset_degrees = sanitize_oval_radius_offset_degrees(normalized.radius_offset_degrees);
    if (normalized.points.empty()) {
        normalized.points =
            make_default_oval_anchor_points(0, 0, normalized.width_radius_x, normalized.height_radius_z, kDefaultOvalPointCount);
    } else {
        for (auto& point : normalized.points) {
            point.angle_degrees = normalize_angle_degrees(point.angle_degrees);
            if (!std::isfinite(point.rotation_degrees)) {
                point.rotation_degrees = 0.0f;
            }
            if (!std::isfinite(point.depth_offset)) {
                point.depth_offset = 0.0f;
            }
            recompute_oval_point_position(point, normalized.width_radius_x, normalized.height_radius_z);
            point.tags = normalize_tag_list(point.tags);
            point.anti_tags = normalize_tag_list(point.anti_tags);
        }
        normalize_and_sort_oval_points(normalized.points);
        if (normalized.points.empty()) {
            normalized.points =
                make_default_oval_anchor_points(0, 0, normalized.width_radius_x, normalized.height_radius_z, kDefaultOvalPointCount);
        }
    }
    {
        std::unordered_set<std::string> seen_legacy;
        std::vector<std::string> filtered_legacy;
        filtered_legacy.reserve(normalized.legacy_names.size());
        for (const auto& legacy_name : normalized.legacy_names) {
            const std::string trimmed = trim_copy(legacy_name);
            if (trimmed.empty() || trimmed == normalized.name) {
                continue;
            }
            if (seen_legacy.insert(trimmed).second) {
                filtered_legacy.push_back(trimmed);
            }
        }
        normalized.legacy_names = std::move(filtered_legacy);
    }
    const std::string normalized_name = normalized.name;

    auto existing_it = std::find_if(updated.begin(),
                                    updated.end(),
                                    [&](const OvalAnchorMapping& current) { return current.name == normalized.name; });
    if (existing_it != updated.end()) {
        *existing_it = std::move(normalized);
    } else {
        updated.push_back(std::move(normalized));
    }

    const nlohmann::json after_payload = build_oval_anchor_mappings_payload(updated);
    if (after_payload == before_payload) {
        return ensure_oval_center_anchors_for_mapping(normalized_name);
    }

    oval_anchor_mappings = std::move(updated);
    (void)ensure_oval_center_anchors_for_mapping(normalized_name);
    sync_oval_anchor_mappings_info_json();
    return true;
}

bool AssetInfo::remove_oval_anchor_mapping(const std::string& mapping_name) {
    const std::string trimmed_name = trim_copy(mapping_name);
    if (trimmed_name.empty()) {
        return false;
    }

    const nlohmann::json before_payload = oval_anchor_mappings_payload();
    std::vector<OvalAnchorMapping> updated = parse_oval_anchor_mappings_payload(before_payload, name);
    auto remove_it = std::find_if(updated.begin(),
                                  updated.end(),
                                  [&](const OvalAnchorMapping& mapping) { return mapping.name == trimmed_name; });
    if (remove_it == updated.end()) {
        return false;
    }

    std::unordered_set<std::string> center_anchor_names;
    auto append_center_name = [&](const std::string& source_name) {
        const std::string center_name = oval_center_anchor_name(source_name);
        if (!center_name.empty()) {
            center_anchor_names.insert(center_name);
        }
    };
    append_center_name(remove_it->name);
    for (const auto& legacy_name : remove_it->legacy_names) {
        append_center_name(legacy_name);
    }

    updated.erase(remove_it);

    const nlohmann::json after_payload = build_oval_anchor_mappings_payload(updated);
    const bool oval_changed = (after_payload != before_payload);

    bool center_anchor_changed = false;
    if (!center_anchor_names.empty()) {
        for (auto& [animation_id, animation] : animations) {
            (void)animation_id;
            for (std::size_t path_index = 0; path_index < animation.movement_path_count(); ++path_index) {
                auto& path = animation.movement_path(path_index);
                for (auto& frame : path) {
                    if (remove_anchor_in_frame(frame.anchor_points, center_anchor_names)) {
                        frame.rebuild_anchor_lookup();
                        center_anchor_changed = true;
                    }
                }
            }
        }

        if (!anims_json_.is_object()) {
            anims_json_ = nlohmann::json::object();
        }
        if (!info_json_.is_object()) {
            info_json_ = nlohmann::json::object();
        }
        if (!info_json_.contains("animations") || !info_json_["animations"].is_object()) {
            info_json_["animations"] = nlohmann::json::object();
        }

        for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            auto anchor_points_it = it.value().find("anchor_points");
            if (anchor_points_it == it.value().end() || !anchor_points_it->is_array()) {
                continue;
            }
            for (auto& frame_anchors_json : *anchor_points_it) {
                center_anchor_changed =
                    remove_anchor_in_frame_json(frame_anchors_json, center_anchor_names) || center_anchor_changed;
            }
        }
    }

    if (!oval_changed && !center_anchor_changed) {
        return false;
    }

    if (oval_changed) {
        oval_anchor_mappings = std::move(updated);
        sync_oval_anchor_mappings_info_json();
    }
    if (center_anchor_changed) {
        info_json_["animations"] = anims_json_;
    }
    return true;
}

bool AssetInfo::rename_oval_anchor_mapping(const std::string& old_name,
                                           const std::string& new_name,
                                           bool append_legacy_alias) {
    const std::string trimmed_old = trim_copy(old_name);
    const std::string trimmed_new = trim_copy(new_name);
    if (trimmed_old.empty() || trimmed_new.empty()) {
        return false;
    }
    if (trimmed_old == trimmed_new) {
        return false;
    }

    const nlohmann::json before_payload = oval_anchor_mappings_payload();
    std::vector<OvalAnchorMapping> updated = parse_oval_anchor_mappings_payload(before_payload, name);

    auto old_it = std::find_if(updated.begin(),
                               updated.end(),
                               [&](const OvalAnchorMapping& mapping) { return mapping.name == trimmed_old; });
    if (old_it == updated.end()) {
        return false;
    }
    const auto conflict_it = std::find_if(updated.begin(),
                                          updated.end(),
                                          [&](const OvalAnchorMapping& mapping) { return mapping.name == trimmed_new; });
    if (conflict_it != updated.end()) {
        return false;
    }

    old_it->name = trimmed_new;
    if (append_legacy_alias && trimmed_old != trimmed_new) {
        auto has_legacy = std::find(old_it->legacy_names.begin(), old_it->legacy_names.end(), trimmed_old);
        if (has_legacy == old_it->legacy_names.end()) {
            old_it->legacy_names.push_back(trimmed_old);
        }
    }
    old_it->legacy_names.erase(std::remove(old_it->legacy_names.begin(),
                                           old_it->legacy_names.end(),
                                           trimmed_new),
                               old_it->legacy_names.end());

    const nlohmann::json after_payload = build_oval_anchor_mappings_payload(updated);
    const bool oval_changed = (after_payload != before_payload);
    bool candidate_changed = false;
    bool center_anchor_changed = false;
    if (trimmed_old != trimmed_new) {
        candidate_changed = rename_anchor_point_child_candidate(trimmed_old, trimmed_new);
        center_anchor_changed = rename_oval_center_anchors_for_mapping(trimmed_old, trimmed_new, true);
    }

    if (!oval_changed && !candidate_changed && !center_anchor_changed) {
        return false;
    }
    if (oval_changed) {
        oval_anchor_mappings = std::move(updated);
        sync_oval_anchor_mappings_info_json();
    }
    return true;
}

const AssetInfo::OvalAnchorMapping* AssetInfo::find_oval_anchor_mapping(const std::string& mapping_name,
                                                                        bool include_legacy_aliases) const {
    if (mapping_name.empty()) {
        return nullptr;
    }
    auto direct_it = std::find_if(oval_anchor_mappings.begin(),
                                  oval_anchor_mappings.end(),
                                  [&](const OvalAnchorMapping& mapping) { return mapping.name == mapping_name; });
    if (direct_it != oval_anchor_mappings.end()) {
        return &(*direct_it);
    }
    if (!include_legacy_aliases) {
        return nullptr;
    }
    for (const auto& mapping : oval_anchor_mappings) {
        if (std::find(mapping.legacy_names.begin(), mapping.legacy_names.end(), mapping_name) !=
            mapping.legacy_names.end()) {
            return &mapping;
        }
    }
    return nullptr;
}

std::string AssetInfo::oval_center_anchor_name_for_mapping(const std::string& mapping_name) {
    return oval_center_anchor_name(mapping_name);
}

bool AssetInfo::ensure_oval_center_anchors_for_mapping(const std::string& mapping_name,
                                                       bool include_legacy_aliases) {
    const std::string trimmed_mapping_name = trim_copy(mapping_name);
    if (trimmed_mapping_name.empty()) {
        return false;
    }

    const OvalAnchorMapping* mapping = find_oval_anchor_mapping(trimmed_mapping_name, include_legacy_aliases);
    if (!mapping) {
        return false;
    }

    std::vector<std::string> center_anchor_names;
    {
        std::unordered_set<std::string> seen_names;
        auto add_center_name = [&](const std::string& source_name) {
            const std::string center_name = oval_center_anchor_name(source_name);
            if (!center_name.empty() && seen_names.insert(center_name).second) {
                center_anchor_names.push_back(center_name);
            }
        };
        add_center_name(mapping->name);
        if (include_legacy_aliases) {
            for (const auto& legacy_name : mapping->legacy_names) {
                add_center_name(legacy_name);
            }
        }
    }
    if (center_anchor_names.empty()) {
        return false;
    }

    bool changed = false;
    for (auto& [animation_id, animation] : animations) {
        (void)animation_id;
        for (std::size_t path_index = 0; path_index < animation.movement_path_count(); ++path_index) {
            auto& path = animation.movement_path(path_index);
            for (auto& frame : path) {
                const SDL_Point frame_dims = resolve_anchor_frame_dimensions(*this, &frame);
                bool frame_changed = false;
                for (const auto& center_name : center_anchor_names) {
                    const DisplacedAssetAnchorPoint default_anchor = make_default_center_anchor(center_name, frame_dims);
                    frame_changed = upsert_anchor_in_frame(frame.anchor_points, default_anchor) || frame_changed;
                }
                if (frame_changed) {
                    frame.rebuild_anchor_lookup();
                    changed = true;
                }
            }
        }
    }

    if (!anims_json_.is_object()) {
        anims_json_ = nlohmann::json::object();
    }
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }
    if (!info_json_.contains("animations") || !info_json_["animations"].is_object()) {
        info_json_["animations"] = nlohmann::json::object();
    }

    const SDL_Point fallback_dims{
        std::max(1, original_canvas_width > 0 ? original_canvas_width : 1),
        std::max(1, original_canvas_height > 0 ? original_canvas_height : 1),
    };

    for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }

        const std::string animation_id = it.key();
        Animation* runtime_animation = nullptr;
        auto runtime_it = animations.find(animation_id);
        if (runtime_it != animations.end()) {
            runtime_animation = &runtime_it->second;
        }

        std::size_t frame_count = 0;
        auto anchor_points_it = it.value().find("anchor_points");
        if (anchor_points_it != it.value().end() && anchor_points_it->is_array()) {
            frame_count = anchor_points_it->size();
        }
        if (frame_count == 0 && runtime_animation && runtime_animation->has_frames()) {
            frame_count = runtime_animation->frame_count();
        }
        if (frame_count == 0) {
            continue;
        }

        normalize_anchor_points_payload(it.value(), frame_count);
        auto normalized_anchor_points_it = it.value().find("anchor_points");
        if (normalized_anchor_points_it == it.value().end() || !normalized_anchor_points_it->is_array()) {
            continue;
        }

        for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
            if (frame_index >= normalized_anchor_points_it->size()) {
                continue;
            }
            nlohmann::json& frame_anchors_json = (*normalized_anchor_points_it)[frame_index];
            if (!frame_anchors_json.is_array()) {
                frame_anchors_json = nlohmann::json::array();
            }

            SDL_Point frame_dims = fallback_dims;
            if (runtime_animation) {
                if (AnimationFrame* frame = runtime_animation->primary_frame_at(frame_index)) {
                    frame_dims = resolve_anchor_frame_dimensions(*this, frame);
                }
            }

            for (const auto& center_name : center_anchor_names) {
                const DisplacedAssetAnchorPoint default_anchor = make_default_center_anchor(center_name, frame_dims);
                if (upsert_anchor_in_frame_json(frame_anchors_json, default_anchor)) {
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        info_json_["animations"] = anims_json_;
    }
    return changed;
}

bool AssetInfo::ensure_oval_center_anchors_for_all_mappings() {
    bool changed = false;
    for (const auto& mapping : oval_anchor_mappings) {
        changed = ensure_oval_center_anchors_for_mapping(mapping.name, true) || changed;
    }
    return changed;
}

bool AssetInfo::rename_oval_center_anchors_for_mapping(const std::string& old_mapping_name,
                                                       const std::string& new_mapping_name,
                                                       bool include_legacy_aliases) {
    const std::string trimmed_old = trim_copy(old_mapping_name);
    const std::string trimmed_new = trim_copy(new_mapping_name);
    if (trimmed_old.empty() || trimmed_new.empty() || trimmed_old == trimmed_new) {
        return false;
    }

    std::vector<std::string> old_center_names;
    {
        std::unordered_set<std::string> seen_old_centers;
        auto add_old = [&](const std::string& source_name) {
            const std::string center_name = oval_center_anchor_name(source_name);
            if (!center_name.empty() && seen_old_centers.insert(center_name).second) {
                old_center_names.push_back(center_name);
            }
        };
        add_old(trimmed_old);
        if (include_legacy_aliases) {
            if (const OvalAnchorMapping* old_mapping = find_oval_anchor_mapping(trimmed_old, true)) {
                for (const auto& legacy_name : old_mapping->legacy_names) {
                    if (legacy_name != trimmed_new) {
                        add_old(legacy_name);
                    }
                }
            }
        }
    }

    const std::string new_center_name = oval_center_anchor_name(trimmed_new);
    if (new_center_name.empty() || old_center_names.empty()) {
        return false;
    }

    bool changed = false;
    for (auto& [animation_id, animation] : animations) {
        (void)animation_id;
        for (std::size_t path_index = 0; path_index < animation.movement_path_count(); ++path_index) {
            auto& path = animation.movement_path(path_index);
            for (auto& frame : path) {
                bool frame_changed = false;
                for (const auto& old_center_name : old_center_names) {
                    frame_changed = rename_anchor_in_frame(frame.anchor_points, old_center_name, new_center_name) || frame_changed;
                }
                if (frame_changed) {
                    frame.rebuild_anchor_lookup();
                    changed = true;
                }
            }
        }
    }

    if (!anims_json_.is_object()) {
        anims_json_ = nlohmann::json::object();
    }
    if (!info_json_.is_object()) {
        info_json_ = nlohmann::json::object();
    }
    if (!info_json_.contains("animations") || !info_json_["animations"].is_object()) {
        info_json_["animations"] = nlohmann::json::object();
    }

    for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        auto anchor_points_it = it.value().find("anchor_points");
        if (anchor_points_it == it.value().end() || !anchor_points_it->is_array()) {
            continue;
        }
        for (auto& frame_anchors_json : *anchor_points_it) {
            if (!frame_anchors_json.is_array()) {
                continue;
            }
            for (const auto& old_center_name : old_center_names) {
                if (rename_anchor_in_frame_json(frame_anchors_json, old_center_name, new_center_name)) {
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        info_json_["animations"] = anims_json_;
    }
    return changed;
}

nlohmann::json AssetInfo::anchor_point_child_candidate_candidates(const std::string& anchor_point_name) const {
    if (anchor_point_name.empty()) {
        nlohmann::json empty_entry = nlohmann::json::object();
        empty_entry["candidates"] = normalize_anchor_candidate_payload(nlohmann::json::object())["candidates"];
        empty_entry["orphan_on_end"] = true;
        return empty_entry;
    }
    for (const auto& entry : anchor_point_child_candidates) {
        if (entry.anchor_point_name == anchor_point_name) {
            nlohmann::json normalized = normalize_anchor_candidate_payload(entry.candidates);
            normalized["orphan_on_end"] = entry.orphan_on_end;
            return normalized;
        }
    }
    nlohmann::json empty_entry = normalize_anchor_candidate_payload(nlohmann::json::object());
    empty_entry["orphan_on_end"] = true;
    return empty_entry;
}

bool AssetInfo::anchor_point_child_candidate_orphan_on_end(const std::string& anchor_point_name) const {
    if (anchor_point_name.empty()) {
        return true;
    }
    for (const auto& entry : anchor_point_child_candidates) {
        if (entry.anchor_point_name == anchor_point_name) {
            return entry.orphan_on_end;
        }
    }
    return true;
}

bool AssetInfo::set_anchor_point_child_candidate_orphan_on_end(const std::string& anchor_point_name, bool orphan_on_end) {
    if (anchor_point_name.empty()) {
        return false;
    }

    const nlohmann::json before_payload = anchor_point_child_candidates_payload();
    std::vector<AnchorChildPointCandidate> updated =
        parse_anchor_point_child_candidates_payload(before_payload);

    auto existing = std::find_if(updated.begin(),
                                 updated.end(),
                                 [&](const AnchorChildPointCandidate& candidate) {
                                     return candidate.anchor_point_name == anchor_point_name;
                                 });
    if (existing != updated.end()) {
        existing->orphan_on_end = orphan_on_end;
    } else {
        AnchorChildPointCandidate created{};
        created.anchor_point_name = anchor_point_name;
        created.candidates = normalize_anchor_candidate_payload(nlohmann::json::object());
        created.orphan_on_end = orphan_on_end;
        updated.push_back(std::move(created));
    }

    const nlohmann::json after_payload = build_anchor_point_child_candidates_payload(updated);
    if (after_payload == before_payload) {
        return false;
    }

    anchor_point_child_candidates = std::move(updated);
    sync_anchor_point_child_candidates_info_json();
    return true;
}

bool AssetInfo::upsert_anchor_point_child_candidate(const std::string& anchor_point_name, const nlohmann::json& candidates) {
    if (anchor_point_name.empty()) {
        return false;
    }

    const nlohmann::json before_payload = anchor_point_child_candidates_payload();
    std::vector<AnchorChildPointCandidate> updated =
        parse_anchor_point_child_candidates_payload(before_payload);

    nlohmann::json normalized_candidates = normalize_anchor_candidate_payload(candidates);
    auto existing = std::find_if(updated.begin(),
                                 updated.end(),
                                 [&](const AnchorChildPointCandidate& candidate) {
                                     return candidate.anchor_point_name == anchor_point_name;
                                 });
    if (existing != updated.end()) {
        // Treat an empty object as "ensure exists" instead of destructive reset.
        const bool request_is_empty_object = candidates.is_object() && candidates.empty();
        if (request_is_empty_object) {
            normalized_candidates = normalize_anchor_candidate_payload(existing->candidates);
        }
        existing->candidates = normalized_candidates;
    } else {
        AnchorChildPointCandidate created{};
        created.anchor_point_name = anchor_point_name;
        created.candidates = normalized_candidates;
        created.orphan_on_end = true;
        updated.push_back(std::move(created));
    }

    const nlohmann::json after_payload = build_anchor_point_child_candidates_payload(updated);
    if (after_payload == before_payload) {
        return false;
    }

    anchor_point_child_candidates = std::move(updated);
    sync_anchor_point_child_candidates_info_json();
    return true;
}

bool AssetInfo::rename_anchor_point_child_candidate(const std::string& old_name, const std::string& new_name) {
    if (old_name.empty() || new_name.empty() || old_name == new_name) {
        return false;
    }

    const nlohmann::json before_payload = anchor_point_child_candidates_payload();
    std::vector<AnchorChildPointCandidate> updated =
        parse_anchor_point_child_candidates_payload(before_payload);

    auto old_it = std::find_if(updated.begin(),
                               updated.end(),
                               [&](const AnchorChildPointCandidate& candidate) {
                                   return candidate.anchor_point_name == old_name;
                               });
    if (old_it == updated.end()) {
        return false;
    }

    auto new_it = std::find_if(updated.begin(),
                               updated.end(),
                               [&](const AnchorChildPointCandidate& candidate) {
                                   return candidate.anchor_point_name == new_name;
                               });

    if (new_it != updated.end()) {
        updated.erase(old_it);
    } else {
        old_it->anchor_point_name = new_name;
    }

    const nlohmann::json after_payload = build_anchor_point_child_candidates_payload(updated);
    if (after_payload == before_payload) {
        return false;
    }

    anchor_point_child_candidates = std::move(updated);
    sync_anchor_point_child_candidates_info_json();
    return true;
}

bool AssetInfo::remove_anchor_point_child_candidate(const std::string& anchor_point_name) {
    if (anchor_point_name.empty()) {
        return false;
    }

    const nlohmann::json before_payload = anchor_point_child_candidates_payload();
    std::vector<AnchorChildPointCandidate> updated =
        parse_anchor_point_child_candidates_payload(before_payload);
    const auto erase_it = std::remove_if(updated.begin(),
                                         updated.end(),
                                         [&](const AnchorChildPointCandidate& candidate) {
                                             return candidate.anchor_point_name == anchor_point_name;
                                         });
    if (erase_it == updated.end()) {
        return false;
    }
    updated.erase(erase_it, updated.end());

    const nlohmann::json after_payload = build_anchor_point_child_candidates_payload(updated);
    if (after_payload == before_payload) {
        return false;
    }

    anchor_point_child_candidates = std::move(updated);
    sync_anchor_point_child_candidates_info_json();
    return true;
}

bool AssetInfo::reconcile_anchor_point_child_candidates(const std::vector<std::string>& canonical_anchor_names) {
    const nlohmann::json before_payload = anchor_point_child_candidates_payload();
    std::vector<AnchorChildPointCandidate> current =
        parse_anchor_point_child_candidates_payload(before_payload);

    std::vector<std::string> canonical_order;
    canonical_order.reserve(canonical_anchor_names.size());
    std::unordered_set<std::string> seen_names;
    for (const std::string& name : canonical_anchor_names) {
        if (name.empty()) {
            continue;
        }
        if (seen_names.insert(name).second) {
            canonical_order.push_back(name);
        }
    }

    std::vector<AnchorChildPointCandidate> reconciled;
    reconciled.reserve(canonical_order.size());
    for (const std::string& name : canonical_order) {
        auto it = std::find_if(current.begin(),
                               current.end(),
                               [&](const AnchorChildPointCandidate& candidate) {
                                   return candidate.anchor_point_name == name;
                               });
        if (it != current.end()) {
            reconciled.push_back(*it);
        } else {
            AnchorChildPointCandidate created{};
            created.anchor_point_name = name;
            created.candidates = nlohmann::json::object();
            created.orphan_on_end = true;
            reconciled.push_back(std::move(created));
        }
    }

    const nlohmann::json after_payload = build_anchor_point_child_candidates_payload(reconciled);
    if (after_payload == before_payload) {
        return false;
    }

    anchor_point_child_candidates = std::move(reconciled);
    sync_anchor_point_child_candidates_info_json();
    return true;
}

bool AssetInfo::remove_area(const std::string& name) {
    bool removed = false;

    areas.erase(std::remove_if(areas.begin(), areas.end(), [&](const NamedArea& na){ return na.name == name; }), areas.end());

    try {
        if (info_json_.contains("areas") && info_json_["areas"].is_array()) {
            nlohmann::json new_arr = nlohmann::json::array();
            for (const auto& entry : info_json_["areas"]) {
                if (entry.is_object() && entry.value("name", std::string{}) == name) {
                    removed = true;
                    continue;
                }
                new_arr.push_back(entry);
            }
            info_json_["areas"] = std::move(new_arr);
        }
    } catch (...) {

    }
    return removed;
}

bool AssetInfo::rename_area(const std::string& old_name, const std::string& new_name) {
    if (old_name.empty() || new_name.empty()) {
        return false;
    }
    if (old_name == new_name) {
        return true;
    }

    auto conflict = std::find_if(areas.begin(), areas.end(), [&](const NamedArea& na) {
        return na.name == new_name;
    });
    if (conflict != areas.end()) {
        return false;
    }

    bool renamed = false;
    for (auto& na : areas) {
        if (na.name == old_name) {
            na.name = new_name;
            if (na.area) {
                na.area->set_name(new_name);
            }
            renamed = true;
        }
    }
    if (!renamed) {
        return false;
    }

    try {
        if (info_json_.contains("areas") && info_json_["areas"].is_array()) {
            for (auto& entry : info_json_["areas"]) {
                if (entry.is_object() && entry.value("name", std::string{}) == old_name) {
                    entry["name"] = new_name;
                }
            }
        }
    } catch (...) {

    }

    return true;
}

std::vector<std::string> AssetInfo::animation_names() const {
	std::vector<std::string> names;
	try {
		if (info_json_.contains("animations") && info_json_["animations"].is_object()) {
			for (auto it = info_json_["animations"].begin(); it != info_json_["animations"].end(); ++it) {
				names.push_back(it.key());
			}
		}
	} catch (...) {

	}
	std::sort(names.begin(), names.end());
	return names;
}

nlohmann::json AssetInfo::animation_payload(const std::string& name) const {
	try {
		if (info_json_.contains("animations") && info_json_["animations"].is_object()) {
			auto it = info_json_["animations"].find(name);
			if (it != info_json_["animations"].end()) {
				return *it;
			}
		}
	} catch (...) {}
	return nlohmann::json::object();
}

bool AssetInfo::upsert_animation(const std::string& name, const nlohmann::json& payload) {
	if (name.empty()) return false;
	try {
		nlohmann::json existing_payload = nlohmann::json::object();
		bool has_existing = false;
		if (anims_json_.is_object() && anims_json_.contains(name) && anims_json_[name].is_object()) {
			existing_payload = normalize_animation_payload(anims_json_[name]);
			has_existing = true;
		} else if (info_json_.is_object() &&
		           info_json_.contains("animations") &&
		           info_json_["animations"].is_object() &&
		           info_json_["animations"].contains(name) &&
		           info_json_["animations"][name].is_object()) {
			existing_payload = normalize_animation_payload(info_json_["animations"][name]);
			has_existing = true;
		}

		nlohmann::json clean_payload = normalize_animation_payload(payload);
		apply_system_enable_flags_to_animation_payload(clean_payload,
		                                              movement_enabled,
		                                              hitbox_enabled,
		                                              attack_box_enabled);
		if (!info_json_.contains("animations") || !info_json_["animations"].is_object()) {
			info_json_["animations"] = nlohmann::json::object();
		}
		info_json_["animations"][name] = clean_payload;

		if (anims_json_.is_null() || !anims_json_.is_object()) anims_json_ = nlohmann::json::object();
		anims_json_[name] = clean_payload;

		if (!has_existing) {
			mark_texture_rebuild_on_close(name, kTextureVariantAll);
            bump_animation_metadata_revision();
		} else {
			const auto variants = classify_texture_rebuild_variants(existing_payload, clean_payload);
			if (variants != kTextureVariantNone) {
				mark_texture_rebuild_on_close(name, variants);
                bump_animation_metadata_revision();
			}
		}
		return true;
	} catch (...) {
		return false;
	}
}

bool AssetInfo::remove_animation(const std::string& name) {
	bool removed = false;
	try {
		if (info_json_.contains("animations") && info_json_["animations"].is_object()) {
			removed = info_json_["animations"].erase(name) > 0;
		}
		if (anims_json_.is_object()) {
			anims_json_.erase(name);
		}
		if (start_animation == name) {
			start_animation.clear();
			info_json_["start"] = start_animation;
		}
	} catch (...) {
		removed = false;
	}
	if (removed) {
		mark_bundle_refresh_on_close();
        bump_animation_metadata_revision();
	}
	return removed;
}

bool AssetInfo::rename_animation(const std::string& old_name, const std::string& new_name) {
	if (old_name.empty() || new_name.empty() || old_name == new_name) return false;
	try {
		nlohmann::json payload;
		bool found = false;
		if (info_json_.contains("animations") && info_json_["animations"].is_object()) {
			auto it = info_json_["animations"].find(old_name);
			if (it != info_json_["animations"].end()) { payload = *it; found = true; }
		}
		if (!found) return false;

		info_json_["animations"][new_name] = payload;
		info_json_["animations"].erase(old_name);
		if (anims_json_.is_null() || !anims_json_.is_object()) anims_json_ = nlohmann::json::object();
		anims_json_[new_name] = payload;
		anims_json_.erase(old_name);
		if (start_animation == old_name) {
			start_animation = new_name;
		 info_json_["start"] = start_animation;
		}
		mark_texture_rebuild_on_close(new_name, kTextureVariantAll);
		mark_bundle_refresh_on_close();
        bump_animation_metadata_revision();
		return true;
	} catch (...) {
		return false;
	}
}

void AssetInfo::set_start_animation_name(const std::string& name) {
        try {
                if (start_animation == name) {
                    return;
                }
                start_animation = name;
                info_json_["start"] = name;
                bump_animation_metadata_revision();
        } catch (...) {

        }
}

bool AssetInfo::reload_animations_from_disk() {
    auto animation_reload_snapshot = [this]() {
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["start"] = start_animation;
        snapshot["animations"] = anims_json_.is_object() ? anims_json_ : nlohmann::json::object();
        return snapshot;
    };
    const nlohmann::json previous_snapshot = animation_reload_snapshot();
    auto apply_payload = [this](const nlohmann::json& payload) -> bool {
        if (!payload.is_object()) {
            return false;
        }

        movement_enabled = payload.contains(kMovementEnabledKey) && payload[kMovementEnabledKey].is_boolean()
            ? payload[kMovementEnabledKey].get<bool>()
            : false;
        attack_box_enabled = payload.contains(kAttackBoxEnabledKey) && payload[kAttackBoxEnabledKey].is_boolean()
            ? payload[kAttackBoxEnabledKey].get<bool>()
            : false;
        hitbox_enabled = payload.contains(kHitboxEnabledKey) && payload[kHitboxEnabledKey].is_boolean()
            ? payload[kHitboxEnabledKey].get<bool>()
            : false;
        impassable_enabled = payload.contains(kImpassableEnabledKey) && payload[kImpassableEnabledKey].is_boolean()
            ? payload[kImpassableEnabledKey].get<bool>()
            : false;
        floor_boxes_enabled = payload.contains(kFloorBoxesEnabledKey) && payload[kFloorBoxesEnabledKey].is_boolean()
            ? payload[kFloorBoxesEnabledKey].get<bool>()
            : false;
        floor_boxes.clear();
        if (floor_boxes_enabled && payload.contains(kFloorBoxesKey) && payload[kFloorBoxesKey].is_array()) {
            floor_boxes = parse_floor_boxes_payload(payload[kFloorBoxesKey]);
        }
        impassable_shapes.clear();
        if (impassable_enabled && payload.contains(kImpassableShapesKey) && payload[kImpassableShapesKey].is_array()) {
            impassable_shapes = parse_impassable_shapes_payload(payload[kImpassableShapesKey]);
        }

        load_animations(payload);

        std::string new_start = start_animation;
        std::string candidate;
        if (extract_start_value(payload, candidate)) {
            new_start = std::move(candidate);
        }
        if (new_start.empty()) {
            new_start = start_animation;
        }
        if (new_start.empty()) {
            new_start = "default";
        }

        start_animation = new_start;
        if (!info_json_.is_object()) {
            info_json_ = nlohmann::json::object();
        }
        info_json_[kMovementEnabledKey] = movement_enabled;
        info_json_[kAttackBoxEnabledKey] = attack_box_enabled;
        info_json_[kHitboxEnabledKey] = hitbox_enabled;
        info_json_[kImpassableEnabledKey] = impassable_enabled;
        info_json_[kFloorBoxesEnabledKey] = floor_boxes_enabled;
        info_json_.erase("impassable_box_enabled");
        info_json_.erase("impassable_boxes");
        if (floor_boxes_enabled) {
            const nlohmann::json floor_payload = encode_floor_boxes_payload(floor_boxes);
            if (!floor_payload.empty()) {
                info_json_[kFloorBoxesKey] = floor_payload;
            } else {
                info_json_.erase(kFloorBoxesKey);
            }
        } else {
            info_json_.erase(kFloorBoxesKey);
        }
        if (impassable_enabled) {
            const nlohmann::json impassable_payload = encode_impassable_shapes_payload(impassable_shapes);
            if (!impassable_payload.empty()) {
                info_json_[kImpassableShapesKey] = impassable_payload;
            } else {
                info_json_.erase(kImpassableShapesKey);
            }
        } else {
            info_json_.erase(kImpassableShapesKey);
        }
        info_json_["start"] = start_animation;
        return true;
};

    auto& provider = manifest_store_provider_slot();
    if (!provider) {
        return false;
    }
    devmode::core::ManifestStore* store = provider();
    if (!store) {
        return false;
    }
    auto view = store->get_asset(name);
    if (!view || !view.data) {
        return false;
    }
    const bool applied = apply_payload(*view.data);
    if (applied) {
        const nlohmann::json updated_snapshot = animation_reload_snapshot();
        if (updated_snapshot != previous_snapshot) {
            bump_animation_metadata_revision();
        }
    }
    return applied;
}

AssetInfo::AnimationUpdateResult AssetInfo::update_animation_properties_detailed(
    const std::string& animation_name,
    const nlohmann::json& properties) {
    AnimationUpdateResult result{};
    if (animation_name.empty() || !properties.is_object()) {
        return result;
    }

    try {
        if (!anims_json_.is_object()) {
            anims_json_ = nlohmann::json::object();
        }

        nlohmann::json existing_animation = nlohmann::json::object();
        bool has_existing_animation = false;
        if (anims_json_.contains(animation_name) && anims_json_[animation_name].is_object()) {
            existing_animation = normalize_animation_payload(anims_json_[animation_name]);
            has_existing_animation = true;
        } else if (info_json_.is_object() &&
                   info_json_.contains("animations") &&
                   info_json_["animations"].is_object() &&
                   info_json_["animations"].contains(animation_name) &&
                   info_json_["animations"][animation_name].is_object()) {
            existing_animation = normalize_animation_payload(info_json_["animations"][animation_name]);
            has_existing_animation = true;
        }

        nlohmann::json updated_animation = normalize_animation_payload(properties);
        if (has_existing_animation) {
            for (auto& [key, value] : existing_animation.items()) {
                if (!updated_animation.contains(key)) {
                    updated_animation[key] = value;
                }
            }
        }
        apply_system_enable_flags_to_animation_payload(updated_animation,
                                                       movement_enabled,
                                                       hitbox_enabled,
                                                       attack_box_enabled);

        const bool should_set_start =
            properties.contains("start") && properties["start"].is_boolean() && properties["start"].get<bool>();
        const bool animation_changed = !has_existing_animation || existing_animation != updated_animation;
        const bool start_changed = should_set_start && start_animation != animation_name;
        if (!animation_changed && !start_changed) {
            return result;
        }

        anims_json_[animation_name] = updated_animation;

        if (!info_json_.is_object()) {
            info_json_ = nlohmann::json::object();
        }
        if (!info_json_.contains("animations") || !info_json_["animations"].is_object()) {
            info_json_["animations"] = nlohmann::json::object();
        }
        info_json_["animations"][animation_name] = updated_animation;

        if (should_set_start) {
            start_animation = animation_name;
            info_json_["start"] = start_animation;
        }
        bump_animation_metadata_revision();

        result.changed = true;
        result.animation_changed = animation_changed;
        result.start_changed = start_changed;

        if (animation_changed) {
            if (!has_existing_animation) {
                result.variant_mask = kTextureVariantAll;
            } else {
                const auto variants = classify_texture_rebuild_variants(existing_animation, updated_animation);
                result.variant_mask = variants;
            }
            if (result.variant_mask != kTextureVariantNone) {
                mark_texture_rebuild_on_close(animation_name, result.variant_mask);
            }
        }

        result.structural = result.variant_mask != kTextureVariantNone;
        return result;
    } catch (const std::exception& e) {
        std::cerr << "[AssetInfo] Failed to update animation properties for '" << animation_name << "': " << e.what() << std::endl;
        return AnimationUpdateResult{};
    }
}

bool AssetInfo::update_animation_properties(const std::string& animation_name, const nlohmann::json& properties) {
    return update_animation_properties_detailed(animation_name, properties).changed;
}

void AssetInfo::bump_animation_metadata_revision() {
    ++animation_metadata_revision_;
}

AssetInfo::AnimationLoadResult AssetInfo::loadAnimationsDetailed(SDL_Renderer* renderer,
                                                                  bool include_all_animations,
                                                                  bool assume_cache_ready,
                                                                  bool allow_placeholder_fallback) {
    AnimationLoadResult result{};
    if (!anims_json_.is_object()) return result;
    result.attempted = true;
    const std::string asset_name = this->name;

    auto parse_source_animation = [](const nlohmann::json& payload) -> std::optional<std::string> {
        if (!payload.contains("source") || !payload["source"].is_object()) {
            return std::nullopt;
        }
        const auto& source = payload["source"];
        try {
            const std::string kind = source.value("kind", std::string{});
            if (kind != "animation") {
                return std::nullopt;
            }
            const std::string name = source.value("name", std::string{});
            if (name.empty()) {
                return std::nullopt;
            }
            return name;
        } catch (...) {
            return std::nullopt;
        }
    };

    std::unordered_set<std::string> selected_animation_names;
    if (include_all_animations) {
        for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
            selected_animation_names.insert(it.key());
        }
    } else {
        if (!start_animation.empty() && anims_json_.contains(start_animation)) {
            selected_animation_names.insert(start_animation);
        }
        if (anims_json_.contains("default")) {
            selected_animation_names.insert("default");
        }
        if (selected_animation_names.empty() && !anims_json_.empty()) {
            selected_animation_names.insert(anims_json_.begin().key());
        }

        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<std::string> snapshot(selected_animation_names.begin(),
                                              selected_animation_names.end());
            for (const auto& anim_name : snapshot) {
                auto it = anims_json_.find(anim_name);
                if (it == anims_json_.end() || !it->is_object()) {
                    continue;
                }
                auto source_name = parse_source_animation(*it);
                if (source_name && !source_name->empty()) {
                    if (selected_animation_names.insert(*source_name).second) {
                        changed = true;
                    }
                }
            }
        }
    }

    auto should_include_animation = [&](const std::string& anim_name) {
        if (include_all_animations) {
            return true;
        }
        return selected_animation_names.find(anim_name) != selected_animation_names.end();
    };
    auto is_folder_source_animation = [](const nlohmann::json& payload) {
        if (!payload.is_object()) {
            return false;
        }
        if (!payload.contains("source") || !payload["source"].is_object()) {
            return false;
        }
        try {
            return payload["source"].value("kind", std::string{}) == "folder";
        } catch (...) {
            return false;
        }
    };
    auto prebuilt_frames_are_usable_for_folder_animation = [](const PrebuiltAnimationFrames& prebuilt) {
        if (prebuilt.frames.empty()) {
            return false;
        }
        for (const auto& frame : prebuilt.frames) {
            for (SDL_Texture* texture : frame.textures) {
                if (texture) {
                    return true;
                }
            }
        }
        return false;
    };
    auto join_animation_names = [](const std::vector<std::string>& names) {
        if (names.empty()) {
            return std::string{};
        }
        std::ostringstream joined;
        const std::size_t limit = std::min<std::size_t>(names.size(), 6);
        for (std::size_t idx = 0; idx < limit; ++idx) {
            if (idx != 0) {
                joined << ", ";
            }
            joined << names[idx];
        }
        if (names.size() > limit) {
            joined << ", ...";
        }
        return joined.str();
    };
    auto cached_prebuilt_covers_selected_folder_animations =
        [&](const std::unordered_map<std::string, PrebuiltAnimationFrames>& prebuilt_frames_map,
            std::vector<std::string>* unusable_animations = nullptr) {
            bool all_usable = true;
            for (const auto& animation_name : selected_animation_names) {
                auto it = anims_json_.find(animation_name);
                if (it == anims_json_.end() || !it->is_object()) {
                    continue;
                }
                if (!is_folder_source_animation(*it)) {
                    continue;
                }

                auto prebuilt_it = prebuilt_frames_map.find(animation_name);
                if (prebuilt_it == prebuilt_frames_map.end() ||
                    !prebuilt_frames_are_usable_for_folder_animation(prebuilt_it->second)) {
                    all_usable = false;
                    if (unusable_animations) {
                        unusable_animations->push_back(animation_name);
                    }
                }
            }
            return all_usable;
        };
    auto bundle_contains_transparent_placeholder_for_selected_folder_animation =
        [&](const CacheManager::BundleData& bundle) {
            auto is_transparent_placeholder_layer = [](const CacheManager::BundleFrameLayer& layer) {
                return layer.width == 1 &&
                       layer.height == 1 &&
                       layer.pitch == 4 &&
                       layer.pixels.size() == 4 &&
                       std::all_of(layer.pixels.begin(), layer.pixels.end(), [](std::uint8_t byte) {
                           return byte == 0;
                       });
            };

            for (const auto& bundle_animation : bundle.animations) {
                if (selected_animation_names.find(bundle_animation.name) == selected_animation_names.end()) {
                    continue;
                }
                auto anim_it = anims_json_.find(bundle_animation.name);
                if (anim_it == anims_json_.end() || !anim_it->is_object() || !is_folder_source_animation(*anim_it)) {
                    continue;
                }
                for (const auto& frame : bundle_animation.frames) {
                    for (const auto& variant : frame.variants) {
                        if (!variant.use_atlas && is_transparent_placeholder_layer(variant.base)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

    SDL_Texture* dummy_base_sprite = nullptr;
    int dummy_w = 0;
    int dummy_h = 0;
    std::unordered_map<std::string, PrebuiltAnimationFrames> prebuilt_frames;
    CacheManager::BundleData bundle_data;
    bool prebuilt_cache_ready = false;
    if (renderer) {
        PrimaryAssetCache primary_cache(renderer);
        const std::unordered_set<std::string>* filter =
            include_all_animations ? nullptr : &selected_animation_names;
        if (assume_cache_ready) {
            prebuilt_cache_ready = primary_cache.load_cached_only(*this, prebuilt_frames, bundle_data, filter);
            if (prebuilt_cache_ready) {
                std::vector<std::string> unusable_animations;
                if (!cached_prebuilt_covers_selected_folder_animations(prebuilt_frames, &unusable_animations)) {
                    vibble::log::warn("[AssetInfo] Cached-only preload produced incomplete frame caches for '" + asset_name +
                                      "'; forcing rebuild-capable load. Missing/invalid animations: " +
                                      join_animation_names(unusable_animations));
                    prebuilt_cache_ready = false;
                    prebuilt_frames.clear();
                    bundle_data = CacheManager::BundleData{};
                }
            }
            if (!prebuilt_cache_ready) {
                vibble::log::warn("[AssetInfo] Cached-only animation preload failed integrity checks for '" + asset_name +
                                  "'; falling back to rebuild-capable cache load.");
                prebuilt_cache_ready = primary_cache.load_or_build(*this, prebuilt_frames, bundle_data, filter, allow_placeholder_fallback);
                if (!prebuilt_cache_ready) {
                    vibble::log::warn("[AssetInfo] Rebuild-capable cache load also failed for '" + asset_name + "'.");
                }
            }
        } else {
            prebuilt_cache_ready = primary_cache.load_or_build(*this, prebuilt_frames, bundle_data, filter, allow_placeholder_fallback);
        }
    }

    result.cache_ready = prebuilt_cache_ready;
    result.used_placeholder_fallback = allow_placeholder_fallback &&
        bundle_contains_transparent_placeholder_for_selected_folder_animation(bundle_data);

    auto animation_ready = [this](const std::string& name) {
        auto it = animations.find(name);
        if (it == animations.end()) {
            return false;
        }
        const Animation& anim = it->second;
        return anim.number_of_frames > 0 && anim.has_frames();
};

    for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
        if (!should_include_animation(it.key())) {
            continue;
        }
        animations[it.key()];
    }

    std::filesystem::path cache_root = std::filesystem::path("cache") / this->name / "animations";
    auto load_single = [&](const std::string& name, const nlohmann::json& json) {
        Animation& anim = animations[name];
        PrebuiltAnimationFrames* prebuilt = nullptr;
        auto pre_it = prebuilt_frames.find(name);
        if (pre_it != prebuilt_frames.end()) {
            prebuilt = &pre_it->second;
        }
        AnimationLoader::load(anim, name, json, *this, dir_path_, cache_root.string(), scale_factor, renderer, dummy_base_sprite, dummy_w, dummy_h, original_canvas_width, original_canvas_height, false, nullptr, prebuilt);
};

    std::vector<std::pair<std::string, nlohmann::json>> deferred;

    for (auto it = anims_json_.begin(); it != anims_json_.end(); ++it) {
        const std::string name = it.key();
        const auto& json       = it.value();
        if (!should_include_animation(name)) {
            continue;
        }

        auto source_name = parse_source_animation(json);
        const bool needs_source = source_name.has_value() && *source_name != name;
        if (needs_source && !animation_ready(*source_name)) {
            deferred.emplace_back(name, json);
            continue;
        }

        load_single(name, json);
    }

    std::size_t safety_counter = deferred.size() + 1;
    while (!deferred.empty() && safety_counter-- > 0) {
        bool progress = false;
        for (auto it = deferred.begin(); it != deferred.end();) {
            auto source_name = parse_source_animation(it->second);
            const bool ready = !source_name || source_name->empty() || *source_name == it->first || animation_ready(*source_name);
            if (!ready) {
                ++it;
                continue;
            }

            load_single(it->first, it->second);
            it = deferred.erase(it);
            progress = true;
        }

        if (!progress) {
            break;
        }
    }

    for (const auto& pending : deferred) {
        auto source_name = parse_source_animation(pending.second);
        if (source_name) {
            std::cout << "[AssetInfo] Loading derived animation '" << pending.first
                      << "' without ready source '" << *source_name << "'\n";
        } else {
            std::cout << "[AssetInfo] Loading animation '" << pending.first << "'\n";
        }
        load_single(pending.first, pending.second);
    }

    auto runtime_folder_animation_has_texture = [this](const std::string& animation_name) {
        auto anim_it = animations.find(animation_name);
        if (anim_it == animations.end()) {
            return false;
        }
        const Animation& anim = anim_it->second;
        if (anim.cached_frame_count() == 0) {
            return false;
        }
        for (const auto& frame : anim.cached_frames()) {
            for (SDL_Texture* texture : frame.textures) {
                if (texture) {
                    return true;
                }
            }
        }
        return false;
    };

    for (const auto& animation_name : selected_animation_names) {
        auto anim_it = anims_json_.find(animation_name);
        if (anim_it == anims_json_.end() || !anim_it->is_object() || !is_folder_source_animation(*anim_it)) {
            continue;
        }
        if (!runtime_folder_animation_has_texture(animation_name)) {
            result.missing_runtime_frame_animations.push_back(animation_name);
        }
    }
    std::sort(result.missing_runtime_frame_animations.begin(), result.missing_runtime_frame_animations.end());
    result.missing_runtime_frame_animations.erase(
        std::unique(result.missing_runtime_frame_animations.begin(), result.missing_runtime_frame_animations.end()),
        result.missing_runtime_frame_animations.end());

    if (!result.missing_runtime_frame_animations.empty()) {
        const std::string policy = allow_placeholder_fallback
            ? "placeholder fallback policy was enabled but no usable runtime textures were produced"
            : "required folder animation(s) failed to produce usable runtime textures";
        vibble::log::warn("[AssetInfo] Animation load incomplete for asset '" + asset_name +
                          "': " + policy + "; missing/invalid animations: " +
                          join_animation_names(result.missing_runtime_frame_animations));
    }

    return result;
}

void AssetInfo::loadAnimations(SDL_Renderer* renderer, bool include_all_animations, bool assume_cache_ready) {
    (void)loadAnimationsDetailed(renderer, include_all_animations, assume_cache_ready, true);
}

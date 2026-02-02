#include "render/render.hpp"

#include "asset/asset_library.hpp"
#include "asset/asset_info.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "map_generation/map_layers_geometry.hpp"
#include "utils/log.hpp"

#include <memory>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace render_pipeline {
namespace {

using nlohmann::json;

struct RoomCandidate {
    double adjusted_area = 0.0;
    bool   is_trail      = false;
    bool   is_spawn      = false;
    std::vector<std::string> assets;
};

struct AssetScaleRange {
    float min_scale = std::numeric_limits<float>::max();
    float max_scale = 0.0f;
};

constexpr double kBaseRatio        = 1.1;
constexpr double kMinScaleClamp    = 0.05;
constexpr double kMaxScaleClamp    = 2.0;
constexpr double kDefaultAspect    = 16.0 / 9.0;
constexpr double kSpawnFallbackMin = kBaseRatio * 0.9;
constexpr double kSpawnFallbackMax = kBaseRatio * 1.05;

struct RoomDimensions {
    int width  = 0;
    int height = 0;
};

int infer_radius_from_dims(int w_min, int w_max, int h_min, int h_max) {
    int diameter = 0;
    diameter = std::max(diameter, std::max(w_min, w_max));
    diameter = std::max(diameter, std::max(h_min, h_max));
    if (diameter <= 0) {
        return 0;
    }
    return std::max(1, diameter / 2);
}

RoomDimensions compute_room_dimensions(const json& entry) {
    RoomDimensions dims{};
    if (!entry.is_object()) {
        return dims;
    }

    int min_w = entry.value("min_width", 64);
    int max_w = entry.value("max_width", min_w);
    int min_h = entry.value("min_height", 64);
    int max_h = entry.value("max_height", min_h);
    int radius = entry.value("radius", -1);

    std::string geometry = entry.value("geometry", std::string{"square"});
    std::string lowered  = geometry;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (lowered == "circle") {
        if (radius <= 0) {
            radius = infer_radius_from_dims(min_w, max_w, min_h, max_h);
        }
        if (radius <= 0) {
            radius = 1;
        }
        min_w = max_w = min_h = max_h = radius * 2;
    }

    dims.width  = std::max(min_w, max_w);
    dims.height = std::max(min_h, max_h);
    if (dims.width <= 0) {
        dims.width = 1;
    }
    if (dims.height <= 0) {
        dims.height = 1;
    }
    return dims;
}

double adjusted_area_for_dimensions(const RoomDimensions& dims, double aspect) {
    if (dims.width <= 0 || dims.height <= 0) {
        return 0.0;
    }
    double width  = static_cast<double>(dims.width);
    double height = static_cast<double>(dims.height);
    const double current_aspect = width / height;
    double target_w             = width;
    double target_h             = height;
    if (aspect <= 0.0) {
        aspect = kDefaultAspect;
    }
    if (current_aspect < aspect) {
        target_w = std::round(height * aspect);
    } else if (current_aspect > aspect) {
        target_h = std::round(width / aspect);
    }
    return std::max(1.0, target_w) * std::max(1.0, target_h);
}

std::vector<std::string> gather_spawn_group_assets(const json& node) {
    const json* groups = nullptr;
    if (node.is_array()) {
        groups = &node;
    } else if (node.is_object()) {
        auto it = node.find("spawn_groups");
        if (it != node.end() && it->is_array()) {
            groups = &(*it);
        }
    }
    if (!groups) {
        return {};
    }

    std::vector<std::string> result;
    for (const auto& group : *groups) {
        if (!group.is_object()) {
            continue;
        }
        auto cand_it = group.find("candidates");
        if (cand_it == group.end() || !cand_it->is_array()) {
            continue;
        }
        for (const auto& candidate : *cand_it) {
            if (!candidate.is_object()) {
                continue;
            }
            std::string name = candidate.value("name", std::string{});
            if (name.empty()) {
                continue;
            }
            if (name == "null") {
                continue;
            }
            result.push_back(std::move(name));
        }
    }
    return result;
}

float safe_scale_factor(const std::shared_ptr<AssetInfo>& info) {
    if (!info) {
        return 1.0f;
    }
    const float factor = info->scale_factor;
    if (!(factor > 0.0f) || !std::isfinite(factor)) {
        return 1.0f;
    }
    return factor;
}

std::vector<float> build_variant_steps(float min_scale, float max_scale) {
    std::vector<float> steps;
    if (!(max_scale > 0.0f) || !std::isfinite(max_scale)) {
        return steps;
    }

    const float clamped_max = std::clamp(max_scale, 0.05f, 1.0f);
    const float clamped_min = std::clamp(min_scale, 0.05f, clamped_max);

    steps.push_back(1.0f);

    float upper_candidate = std::min(clamped_max, 0.98f);
    if (upper_candidate < clamped_min) {
        upper_candidate = clamped_min;
    }

    if (std::fabs(upper_candidate - 1.0f) > 1e-3f) {
        steps.push_back(upper_candidate);
    }

    const float base_for_mid = std::max(clamped_min, std::min(upper_candidate, 0.99f));
    if (base_for_mid > clamped_min + 1e-4f) {
        const float mid = std::clamp(std::sqrt(clamped_min * base_for_mid), clamped_min, base_for_mid);
        if (std::fabs(mid - 1.0f) > 1e-3f && std::fabs(mid - upper_candidate) > 1e-3f) {
            steps.push_back(mid);
        }
    }

    if (std::fabs(clamped_min - 1.0f) > 1e-3f) {
        steps.push_back(clamped_min);
    }
    return steps;
}

std::uint64_t compute_revision(const std::string& asset_name,
                               float min_scale,
                               float max_scale,
                               const std::vector<float>& steps) {
    std::ostringstream oss;
    oss << asset_name << '|' << std::setprecision(6) << min_scale << '|' << max_scale;
    for (float step : steps) {
        oss << '|' << std::setprecision(6) << step;
    }
    const std::string payload = oss.str();
    return static_cast<std::uint64_t>(std::hash<std::string>{}(payload));
}

std::string iso_timestamp_now() {
    const auto now      = std::chrono::system_clock::now();
    const auto time_t   = std::chrono::system_clock::to_time_t(now);
    std::tm    tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time_t);
#else
    gmtime_r(&time_t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

}

bool BuildScalingProfiles(const ScalingProfileBuildOptions& options) {
    (void)options;
    vibble::log::info("[ScalingProfileBuilder] Skipping build; using cached scale_* variants already on disk.");
    return true;
}

}

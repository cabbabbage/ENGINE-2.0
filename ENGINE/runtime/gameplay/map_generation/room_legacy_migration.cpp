#include "room_legacy_migration.hpp"

#include <cmath>

namespace {

vibble::weighted_range::WeightedIntRange read_weighted_range_field(
    const nlohmann::json& src,
    const char* key,
    const vibble::weighted_range::WeightedIntRange& fallback) {
    if (!src.is_object() || !src.contains(key)) {
        return fallback;
    }
    return vibble::weighted_range::from_json(src.at(key), fallback);
}

vibble::weighted_range::WeightedIntRange read_weighted_range_legacy_pair(
    const nlohmann::json& src,
    const char* min_key,
    const char* max_key,
    const vibble::weighted_range::WeightedIntRange& fallback,
    bool* used_legacy) {
    if (!src.is_object()) {
        return fallback;
    }
    bool has_min = false;
    bool has_max = false;
    std::int64_t min_value = fallback.center;
    std::int64_t max_value = fallback.center;
    if (src.contains(min_key)) {
        if (src.at(min_key).is_number_integer()) {
            min_value = src.at(min_key).get<std::int64_t>();
            has_min = true;
        } else if (src.at(min_key).is_number_float()) {
            min_value = static_cast<std::int64_t>(std::llround(src.at(min_key).get<double>()));
            has_min = true;
        }
    }
    if (src.contains(max_key)) {
        if (src.at(max_key).is_number_integer()) {
            max_value = src.at(max_key).get<std::int64_t>();
            has_max = true;
        } else if (src.at(max_key).is_number_float()) {
            max_value = static_cast<std::int64_t>(std::llround(src.at(max_key).get<double>()));
            has_max = true;
        }
    }
    if (!has_min && !has_max) {
        return fallback;
    }
    if (!has_min) min_value = max_value;
    if (!has_max) max_value = min_value;
    if (used_legacy) *used_legacy = true;
    return vibble::weighted_range::make_legacy_uniform(min_value, max_value);
}

}  // namespace

namespace room_legacy_migration {

DimensionRanges resolve_dimension_ranges(const nlohmann::json& assets_json,
                                         const vibble::weighted_range::WeightedIntRange& default_range,
                                         const std::function<void(const char*)>& on_legacy_migration) {
    DimensionRanges out;
    out.width = assets_json.contains("width")
        ? read_weighted_range_field(assets_json, "width", default_range)
        : (assets_json.contains("min_width") || assets_json.contains("max_width")
               ? read_weighted_range_legacy_pair(assets_json, "min_width", "max_width", default_range, &out.used_legacy_migration)
               : default_range);
    out.height = assets_json.contains("height")
        ? read_weighted_range_field(assets_json, "height", out.width)
        : (assets_json.contains("min_height") || assets_json.contains("max_height")
               ? read_weighted_range_legacy_pair(assets_json, "min_height", "max_height", out.width, &out.used_legacy_migration)
               : out.width);

    std::string lowered = assets_json.value("geometry", std::string{"square"});
    for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lowered == "circle") {
        if (assets_json.contains("min_radius") || assets_json.contains("max_radius") || assets_json.contains("radius")) {
            out.width = read_weighted_range_legacy_pair(assets_json, "min_radius", "max_radius", default_range, &out.used_legacy_migration);
            out.height = out.width;
            if (on_legacy_migration) on_legacy_migration("legacy_radius_bounds");
        } else {
            out.height = out.width;
        }
    }

    if (out.used_legacy_migration && on_legacy_migration) {
        on_legacy_migration("legacy_dimension_bounds");
    }

    return out;
}

}  // namespace room_legacy_migration

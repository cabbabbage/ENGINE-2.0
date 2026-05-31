#include "room_legacy_migration.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kDefaultRoomSize = 9;

int normalize_size_value(int value,
                         int default_size,
                         int min_size,
                         int max_size,
                         bool coerce_out_of_range_to_default) {
    const int lo = std::min(min_size, max_size);
    const int hi = std::max(min_size, max_size);
    if (coerce_out_of_range_to_default && (value < lo || value > hi)) {
        return std::clamp(default_size, lo, hi);
    }
    return std::clamp(value, lo, hi);
}

bool read_json_int_like(const nlohmann::json& src, const char* key, int& out) {
    if (!src.is_object() || !key || !src.contains(key)) {
        return false;
    }
    const auto& value = src.at(key);
    if (value.is_number_integer()) {
        out = value.get<int>();
        return true;
    }
    if (value.is_number_float()) {
        out = static_cast<int>(std::lround(value.get<double>()));
        return true;
    }
    return false;
}
}

namespace room_legacy_migration {

SizeValue resolve_room_size(const nlohmann::json& assets_json,
                            int default_size,
                            int min_size,
                            int max_size,
                            bool coerce_out_of_range_to_default,
                            const std::function<void(const char*)>& on_legacy_migration) {
    SizeValue out;
    const int fallback_size = default_size > 0 ? default_size : kDefaultRoomSize;
    out.size = normalize_size_value(
        fallback_size,
        fallback_size,
        min_size,
        max_size,
        coerce_out_of_range_to_default);

    int explicit_size = 0;
    if (read_json_int_like(assets_json, "size", explicit_size)) {
        out.size = normalize_size_value(
            explicit_size,
            fallback_size,
            min_size,
            max_size,
            coerce_out_of_range_to_default);
        return out;
    }

    out.used_legacy_migration = true;
    if (on_legacy_migration) {
        on_legacy_migration("legacy_room_size_defaults");
    }
    return out;
}

}  // namespace room_legacy_migration

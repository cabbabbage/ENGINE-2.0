#include "room_legacy_migration.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMinRoomSize = 7;
constexpr int kMaxRoomSize = 20;
constexpr int kDefaultRoomSize = 9;

int clamp_size_value(int value) {
    return std::clamp(value, kMinRoomSize, kMaxRoomSize);
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
                            const std::function<void(const char*)>& on_legacy_migration) {
    SizeValue out;
    out.size = clamp_size_value(default_size > 0 ? default_size : kDefaultRoomSize);

    int explicit_size = 0;
    if (read_json_int_like(assets_json, "size", explicit_size)) {
        out.size = clamp_size_value(explicit_size);
        return out;
    }

    out.used_legacy_migration = true;
    if (on_legacy_migration) {
        on_legacy_migration("legacy_room_size_defaults");
    }
    return out;
}

}  // namespace room_legacy_migration

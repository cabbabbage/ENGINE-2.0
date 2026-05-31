#pragma once

#include <functional>
#include <nlohmann/json.hpp>

namespace room_legacy_migration {

struct SizeValue {
    int size = 9;
    bool used_legacy_migration = false;
};

SizeValue resolve_room_size(const nlohmann::json& assets_json,
                            int default_size,
                            const std::function<void(const char*)>& on_legacy_migration);

}  // namespace room_legacy_migration

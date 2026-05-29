#include "controller_factory.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <unordered_map>

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

#include "animation/controllers/custom_controllers/bartender_controller.hpp"
#include "animation/controllers/custom_controllers/bomb_controller.hpp"
#include "animation/controllers/custom_controllers/carrie_controller.hpp"
#include "animation/controllers/custom_controllers/chest_opening_controller.hpp"
#include "animation/controllers/custom_controllers/davey_controller.hpp"
#include "animation/controllers/custom_controllers/fly_controller.hpp"
#include "animation/controllers/custom_controllers/frog_controller.hpp"
#include "animation/controllers/custom_controllers/gary_controller.hpp"
#include "animation/controllers/custom_controllers/small_spider_controller.hpp"
#include "animation/controllers/custom_controllers/spider_controller.hpp"
#include "animation/controllers/custom_controllers/spider_egg_controller.hpp"
#include "animation/controllers/custom_controllers/vibble_controller.hpp"

#include "animation/controllers/shared/default_custom_controller.hpp"
#include "utils/utils/log.hpp"
#include "utils/utils/string_utils.hpp"

// <<CUSTOM_CONTROLLER_INCLUDE_INSERT_POINT>>

namespace {

using ControllerFactoryFn = std::function<std::unique_ptr<AssetController>(Asset*)>;

std::string asset_debug_name(const Asset* asset) {
    if (!asset || !asset->info || asset->info->name.empty()) {
        return "<unknown asset>";
    }
    return asset->info->name;
}

// Sanitizes an asset name or controller key into a canonical controller key.
// Keeps case but strips invalid characters and ensures the "_controller" suffix.
std::string canonical_controller_key(std::string raw) {
    if (raw.empty()) {
        return {};
    }

    constexpr const char* suffix = "_controller";
    const std::string raw_lower = vibble::strings::to_lower_copy(raw);
    const std::string suffix_text = suffix;

    if (raw_lower.size() >= suffix_text.size() &&
        raw_lower.rfind(suffix_text) == raw_lower.size() - suffix_text.size()) {
        raw = raw.substr(0, raw.size() - suffix_text.size());
    }

    std::string sanitized;
    sanitized.reserve(raw.size() + suffix_text.size());

    bool previous_underscore = false;
    for (char ch : raw) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0) {
            sanitized.push_back(ch);
            previous_underscore = false;
        } else if (!previous_underscore) {
            sanitized.push_back('_');
            previous_underscore = true;
        }
    }

    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }

    if (sanitized.empty()) {
        return {};
    }

    sanitized += suffix_text;
    return sanitized;
}

std::string normalized_controller_key(const std::string& raw) {
    const std::string canonical = canonical_controller_key(raw);
    return canonical.empty() ? std::string{} : vibble::strings::to_lower_copy(canonical);
}

const std::unordered_map<std::string, ControllerFactoryFn>& controller_registry() {
    static const std::unordered_map<std::string, ControllerFactoryFn> registry = {
        {"davey_controller", [](Asset* asset) {
             return std::make_unique<davey_controller>(asset);
         }},
        {"frog_controller", [](Asset* asset) {
             return std::make_unique<frog_controller>(asset);
         }},
        {"carrie_controller", [](Asset* asset) {
             return std::make_unique<carrie_controller>(asset);
         }},
        {"gary_controller", [](Asset* asset) {
             return std::make_unique<gary_controller>(asset);
         }},
        {"bartender_controller", [](Asset* asset) {
             return std::make_unique<bartender_controller>(asset);
         }},
        {"spider_controller", [](Asset* asset) {
             return std::make_unique<spider_controller>(asset);
         }},
        {"bomb_controller", [](Asset* asset) {
             return std::make_unique<bomb_controller>(asset);
         }},
        {"vibble_controller", [](Asset* asset) {
             return std::make_unique<vibble_controller>(asset);
         }},
        {"small_spider_controller", [](Asset* asset) {
             return std::make_unique<small_spider_controller>(asset);
         }},
        {"spider_egg_controller", [](Asset* asset) {
             return std::make_unique<spider_egg_controller>(asset);
         }},
        {"fly_controller", [](Asset* asset) {
             return std::make_unique<fly_controller>(asset);
         }},
        {"chest_opening_controller", [](Asset* asset) {
             return std::make_unique<chest_opening_controller>(asset);
         }},

        // AUTO-GENERATED CUSTOM CONTROLLERS (do not remove marker)
        // <<CUSTOM_CONTROLLER_FACTORY_INSERT_POINT>>
    };

    return registry;
}

std::unique_ptr<AssetController> make_default_controller_safely(Asset* self) {
    if (!self) {
        return nullptr;
    }

    try {
        return std::make_unique<animation_update::custom_controllers::DefaultCustomController>(self, true);
    } catch (const std::bad_alloc& ex) {
        vibble::log::error("Failed to construct fallback DefaultCustomController for asset '" +
                           asset_debug_name(self) + "': " + ex.what());
        return nullptr;
    } catch (const std::exception& ex) {
        vibble::log::error("Failed to construct fallback DefaultCustomController for asset '" +
                           asset_debug_name(self) + "': " + ex.what());
        return nullptr;
    } catch (...) {
        vibble::log::error("Failed to construct fallback DefaultCustomController for asset '" +
                           asset_debug_name(self) + "': unknown exception");
        return nullptr;
    }
}

std::unique_ptr<AssetController> make_player_controller_safely(Asset* self) {
    if (!self) {
        return nullptr;
    }

    try {
        return std::make_unique<vibble_controller>(self);
    } catch (const std::bad_alloc& ex) {
        vibble::log::error("Failed to construct vibble_controller for player asset '" +
                           asset_debug_name(self) + "': " + ex.what());
        return nullptr;
    } catch (const std::exception& ex) {
        vibble::log::error("Failed to construct vibble_controller for player asset '" +
                           asset_debug_name(self) + "': " + ex.what());
        return make_default_controller_safely(self);
    } catch (...) {
        vibble::log::error("Failed to construct vibble_controller for player asset '" +
                           asset_debug_name(self) + "': unknown exception");
        return make_default_controller_safely(self);
    }
}

bool is_player_asset(const Asset* self) {
    return self &&
           self->info &&
           (self->info->type == "Player" || self->info->type == "player");
}

} // namespace

ControllerFactory::ControllerFactory(Assets* assets)
    : assets_(assets) {}

ControllerFactory::~ControllerFactory() = default;

bool ControllerFactory::has_registered_controller_for_asset_name(const std::string& asset_name) {
    return has_registered_controller_for_key(asset_name);
}

bool ControllerFactory::has_registered_controller_for_key(const std::string& key) {
    const std::string normalized = normalized_controller_key(key);
    if (normalized.empty()) {
        return false;
    }

    const auto& registry = controller_registry();
    return registry.find(normalized) != registry.end();
}

std::unique_ptr<AssetController>
ControllerFactory::create_by_key(const std::string& key, Asset* self) const {
    if (!assets_ || !self) {
        return nullptr;
    }

    if (is_player_asset(self)) {
        return make_player_controller_safely(self);
    }

    const std::string asset_key =
        (self->info) ? normalized_controller_key(self->info->name) : std::string{};

    const std::string explicit_key = normalized_controller_key(key);
    const std::string effective_key = explicit_key.empty() ? asset_key : explicit_key;

    if (effective_key.empty()) {
        return make_default_controller_safely(self);
    }

    const auto& registry = controller_registry();
    const auto it = registry.find(effective_key);
    if (it == registry.end()) {
        return make_default_controller_safely(self);
    }

    try {
        return it->second(self);
    } catch (const std::bad_alloc& ex) {
        vibble::log::error("Failed to construct controller '" + effective_key +
                           "' for asset '" + asset_debug_name(self) +
                           "': " + ex.what());

        // Do not allocate a fallback after bad_alloc. Memory is already exhausted
        // or badly fragmented, and trying another allocation can crash setup.
        return nullptr;
    } catch (const std::exception& ex) {
        vibble::log::error("Failed to construct controller '" + effective_key +
                           "' for asset '" + asset_debug_name(self) +
                           "': " + ex.what());
        return make_default_controller_safely(self);
    } catch (...) {
        vibble::log::error("Failed to construct controller '" + effective_key +
                           "' for asset '" + asset_debug_name(self) +
                           "': unknown exception");
        return make_default_controller_safely(self);
    }
}

std::unique_ptr<AssetController>
ControllerFactory::create_for_asset(Asset* self) const {
    if (!assets_ || !self || !self->info) {
        return nullptr;
    }

    if (is_player_asset(self)) {
        return make_player_controller_safely(self);
    }

    return create_by_key(self->info->name, self);
}
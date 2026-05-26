#include "controller_factory.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <unordered_map>

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "animation/controllers/custom_controllers/davey_controller.hpp"
#include "animation/controllers/custom_controllers/vibble_controller.hpp"
#include "animation/controllers/custom_controllers/frog_controller.hpp"
#include "animation/controllers/custom_controllers/bomb_controller.hpp"
#include "animation/controllers/custom_controllers/bartender_controller.hpp"
#include "animation/controllers/custom_controllers/carrie_controller.hpp"
#include "animation/controllers/custom_controllers/gary_controller.hpp"
#include "animation/controllers/custom_controllers/spider_controller.hpp"

#include "animation/controllers/shared/default_custom_controller.hpp"
#include "utils/utils/log.hpp"
#include "utils/utils/string_utils.hpp"

#include "animation/controllers/custom_controllers/fly_controller.hpp"
#include "animation/controllers/custom_controllers/chest_opening_controller.hpp"
// <<CUSTOM_CONTROLLER_INCLUDE_INSERT_POINT>>

namespace {

using ControllerFactoryFn = std::function<std::unique_ptr<AssetController>(Asset*)>;

// Sanitises an asset name or controller key into a canonical controller key.
// Keeps case but strips invalid characters and ensures the "_controller" suffix.
std::string canonical_controller_key(std::string raw) {
        if (raw.empty()) return {};

        // Remove existing suffix so we don't double-append.
        const std::string suffix = "_controller";
        const std::string raw_lower = vibble::strings::to_lower_copy(raw);
        if (raw_lower.size() >= suffix.size() &&
            raw_lower.rfind(suffix) == raw_lower.size() - suffix.size()) {
                raw = raw.substr(0, raw.size() - suffix.size());
        }

        std::string sanitized;
        sanitized.reserve(raw.size() + suffix.size());
        for (char ch : raw) {
                unsigned char uch = static_cast<unsigned char>(ch);
                if (std::isalnum(uch)) {
                        sanitized.push_back(ch);
                } else {
                        sanitized.push_back('_');
                }
        }

        // Trim leading/trailing underscores.
        while (!sanitized.empty() && sanitized.front() == '_') sanitized.erase(sanitized.begin());
        while (!sanitized.empty() && sanitized.back() == '_') sanitized.pop_back();

        if (sanitized.empty()) return {};
        sanitized += suffix;
        return sanitized;
}

std::string normalized_controller_key(const std::string& raw) {
        const std::string canonical = canonical_controller_key(raw);
        return canonical.empty() ? std::string{} : vibble::strings::to_lower_copy(canonical);
}

const std::unordered_map<std::string, ControllerFactoryFn>& controller_registry() {
        static const std::unordered_map<std::string, ControllerFactoryFn> registry = {
                {"davey_controller", [](Asset* asset) { return std::make_unique<davey_controller>(asset); }},
                {"frog_controller", [](Asset* asset) { return std::make_unique<frog_controller>(asset); }},
                {"carrie_controller", [](Asset* asset) { return std::make_unique<carrie_controller>(asset); }},
                {"gary_controller", [](Asset* asset) { return std::make_unique<gary_controller>(asset); }},
                {"bartender_controller", [](Asset* asset) { return std::make_unique<bartender_controller>(asset); }},
                {"spider_controller", [](Asset* asset) { return std::make_unique<spider_controller>(asset); }},
                {"bomb_controller", [](Asset* asset) { return std::make_unique<bomb_controller>(asset); }},
                {"vibble_controller", [](Asset* asset) { return std::make_unique<vibble_controller>(asset); }},
                // AUTO-GENERATED CUSTOM CONTROLLERS (do not remove marker)
                // <<CUSTOM_CONTROLLER_FACTORY_INSERT_POINT>>
        { "fly_controller", [](Asset* asset) {
                return std::make_unique<fly_controller>(asset);
        } },
        };
        return registry;
}

}

ControllerFactory::ControllerFactory(Assets* assets)
: assets_(assets)
{}

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
	if (!assets_ || !self) return nullptr;

        if (self->info && (self->info->type == "Player" || self->info->type == "player")) {
                return std::make_unique<vibble_controller>(self);
        }

        const std::string asset_key = (self->info) ? normalized_controller_key(self->info->name) : std::string{};
        const std::string explicit_key = normalized_controller_key(key);
        const std::string effective_key = explicit_key.empty() ? asset_key : explicit_key;

        if (effective_key.empty()) {
                return std::make_unique<animation_update::custom_controllers::DefaultCustomController>(self, true);
        }

        const auto& registry = controller_registry();
        const auto it = registry.find(effective_key);
        if (it == registry.end()) {
                return std::make_unique<animation_update::custom_controllers::DefaultCustomController>(self, true);
        }

        try {
                return it->second(self);
        } catch (const std::exception& ex) {
                const std::string asset_name = (self->info && !self->info->name.empty()) ? self->info->name
                                                                                          : "<unknown asset>";
                vibble::log::error("Failed to construct controller '" + effective_key + "' for asset '" +
                                   asset_name + "': " + ex.what());
        }
        return std::make_unique<animation_update::custom_controllers::DefaultCustomController>(self, true);
}

std::unique_ptr<AssetController>
ControllerFactory::create_for_asset(Asset* self) const {
        if (!assets_ || !self || !self->info) return nullptr;
        if (self->info->type == "Player" || self->info->type == "player") {
                return std::make_unique<vibble_controller>(self);
        }

        const std::string key = self->info->name;
        return create_by_key(key, self);
}

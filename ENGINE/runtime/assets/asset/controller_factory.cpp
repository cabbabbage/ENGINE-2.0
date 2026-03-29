#include "controller_factory.hpp"

#include <algorithm>
#include <cctype>

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

#include "animation/controllers/shared/custom_asset_controller.hpp"

// <<CUSTOM_CONTROLLER_INCLUDE_INSERT_POINT>>

namespace {

// Sanitises an asset name or controller key into a canonical controller key.
// Keeps case but strips invalid characters and ensures the "_controller" suffix.
std::string canonical_controller_key(std::string raw) {
        if (raw.empty()) return {};

        // Remove existing suffix so we don't double-append.
        auto to_lower = [](const std::string& s) {
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                });
                return out;
        };

        const std::string suffix = "_controller";
        const std::string raw_lower = to_lower(raw);
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

// Lower-case comparison helper for keys (file names are case-insensitive on Windows
// but we want consistent behaviour on other platforms too).
bool key_matches(const std::string& lhs, const std::string& rhs) {
        auto to_lower = [](const std::string& s) {
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                });
                return out;
        };
        return to_lower(lhs) == to_lower(rhs);
}

}

ControllerFactory::ControllerFactory(Assets* assets)
: assets_(assets)
{}

ControllerFactory::~ControllerFactory() = default;

std::unique_ptr<AssetController>
ControllerFactory::create_by_key(const std::string& key, Asset* self) const {
	if (!assets_ || !self) return nullptr;

        const std::string asset_key = (self->info) ? canonical_controller_key(self->info->name) : std::string{};
        const std::string explicit_key = canonical_controller_key(key);
        const std::string effective_key = explicit_key.empty() ? asset_key : explicit_key;

        auto matches = [&](const std::string& candidate) {
                const std::string canonical = canonical_controller_key(candidate);
                if (canonical.empty()) return false;
                return (!effective_key.empty() && key_matches(effective_key, canonical)) ||
                       (!asset_key.empty() && key_matches(asset_key, canonical));
        };

        try {
                if (self->info && (self->info->type == "Player" || self->info->type == "player")) {
                        return std::make_unique<vibble_controller>(self);
                }

                if (matches("davey_controller"))
                        return std::make_unique<davey_controller>(self);
                if (matches("frog_controller"))
                        return std::make_unique<frog_controller>(self);
                if (matches("carrie_controller"))
                        return std::make_unique<carrie_controller>(self);
                if (matches("gary_controller"))
                        return std::make_unique<gary_controller>(self);
                if (matches("bartender_controller"))
                        return std::make_unique<bartender_controller>(self);
                if (matches("spider_controller"))
                        return std::make_unique<spider_controller>(self);
        if (matches("bomb_controller"))
                        return std::make_unique<bomb_controller>(self);

                // AUTO-GENERATED CUSTOM CONTROLLERS (do not remove marker)
                // <<CUSTOM_CONTROLLER_FACTORY_INSERT_POINT>>
        if (matches("vibble_controller"))
                        return std::make_unique<vibble_controller>(self);

        } catch (...) {
        }
        return std::make_unique<CustomAssetController>(self);
}

std::unique_ptr<AssetController>
ControllerFactory::create_for_asset(Asset* self) const {
        if (!assets_ || !self || !self->info) return nullptr;
        if (self->info->type == "Player" || self->info->type == "player") {
                return std::make_unique<vibble_controller>(self);
        }

        const std::string key = canonical_controller_key(self->info->name);
        return create_by_key(key, self);
}

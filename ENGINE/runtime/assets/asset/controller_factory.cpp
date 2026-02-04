#include "controller_factory.hpp"

#include <algorithm>
#include <cctype>

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "animation/controllers/custom_controllers/Davey_controller.hpp"
#include "animation/controllers/custom_controllers/Vibble_controller.hpp"
#include "animation/controllers/custom_controllers/Frog_controller.hpp"
#include "animation/controllers/custom_controllers/Bomb_controller.hpp"
#include "animation/controllers/custom_controllers/Bartender_controller.hpp"
#include "animation/controllers/custom_controllers/Carrie_controller.hpp"
#include "animation/controllers/custom_controllers/Gary_controller.hpp"
#include "animation/controllers/custom_controllers/spider_controller.hpp"

#include "animation/controllers/custom_controllers/default_controller.hpp"

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
                        return std::make_unique<VibbleController>(self);
                }

                if (matches("Davey_controller"))
                        return std::make_unique<DaveyController>(assets_, self);
                if (matches("Frog_controller"))
                        return std::make_unique<FrogController>(assets_, self);
                if (matches("Carrie_controller"))
                        return std::make_unique<CarrieController>(assets_, self);
                if (matches("Gary_controller"))
                        return std::make_unique<GaryController>(assets_, self);
                if (matches("Bartender_controller"))
                        return std::make_unique<BartenderController>(assets_, self);
                if (matches("spider_controller"))
                        return std::make_unique<spiderController>(assets_, self);
                if (matches("Bomb_controller"))
                        return std::make_unique<BombController>(assets_, self);

                // AUTO-GENERATED CUSTOM CONTROLLERS (do not remove marker)
                // <<CUSTOM_CONTROLLER_FACTORY_INSERT_POINT>>

        } catch (...) {
        }
        return std::make_unique<DefaultController>(self);
}

std::unique_ptr<AssetController>
ControllerFactory::create_for_asset(Asset* self) const {
        if (!assets_ || !self || !self->info) return nullptr;
        if (self->info->type == "Player" || self->info->type == "player") {
                return std::make_unique<VibbleController>(self);
        }

        // Prefer controller derived from asset name; fall back to explicit key for legacy assets.
        std::string key = canonical_controller_key(self->info->name);
        if (key.empty()) {
            key = canonical_controller_key(self->info->custom_controller_key);
        }
        return create_by_key(key, self);
}

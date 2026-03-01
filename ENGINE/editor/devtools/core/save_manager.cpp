#include "save_manager.hpp"

#include <algorithm>
#include <iostream>

#include <nlohmann/json.hpp>

#include "devtools/core/manifest_store.hpp"

namespace devmode::core {

void SaveManager::set_manifest_store(ManifestStore* store) {
    store_ = store;
    if (store_) {
        store_->set_require_write_guard(true);
        store_->set_write_violation_sink([](const std::string& kind,
                                            const std::string& key,
                                            const std::string& reason) {
            std::cerr << "[SaveManager] Manifest write for '" << kind << "' key '" << key
                      << "' bypassed SaveManager guard"
                      << (reason.empty() ? "" : std::string(" during '") + reason + "'")
                      << "\n";
        });
    }
}

void SaveManager::set_save_coordinator(DevSaveCoordinator* coordinator) {
    coordinator_ = coordinator;
}

void SaveManager::register_saveable(Saveable saveable) {
    if (saveable.id.empty() || !saveable.save) {
        return;
    }
    auto it = std::find_if(saveables_.begin(), saveables_.end(),
                           [&](const Saveable& existing) { return existing.id == saveable.id; });
    if (it != saveables_.end()) {
        *it = std::move(saveable);
        return;
    }
    saveables_.push_back(std::move(saveable));
}

void SaveManager::unregister_saveable(const std::string& id) {
    if (id.empty()) {
        return;
    }
    saveables_.erase(std::remove_if(saveables_.begin(), saveables_.end(),
                                    [&](const Saveable& s) { return s.id == id; }),
                     saveables_.end());
}

void SaveManager::clear_saveables() {
    saveables_.clear();
}

bool SaveManager::has_dirty_saveables() const {
    for (const auto& saveable : saveables_) {
        if (saveable.is_dirty && saveable.is_dirty()) {
            return true;
        }
    }
    return false;
}

bool SaveManager::save_dirty(DevSaveCoordinator::Priority priority, const std::string& reason) {
    bool any_saved = false;
    ManifestStore::ScopedWriteGuard guard;
    if (store_) {
        guard = store_->scoped_guard(reason.empty() ? "SaveManager batch" : reason);
    }

    std::vector<std::reference_wrapper<const Saveable>> dirty;
    dirty.reserve(saveables_.size());
    for (const auto& saveable : saveables_) {
        if (saveable.is_dirty && !saveable.is_dirty()) {
            continue;
        }
        dirty.push_back(saveable);
    }

    std::sort(dirty.begin(), dirty.end(), [](const auto& a, const auto& b) {
        return static_cast<int>(a.get().stage) < static_cast<int>(b.get().stage);
    });

    for (const auto& ref : dirty) {
        const Saveable& saveable = ref.get();
        if (saveable.save && saveable.save(priority)) {
            any_saved = true;
        }
    }

    if (coordinator_ && priority == DevSaveCoordinator::Priority::Immediate) {
        coordinator_->flush_now(reason);
    } else if (!coordinator_ && store_ && priority == DevSaveCoordinator::Priority::Immediate) {
        store_->flush();
    }
    return any_saved;
}

bool SaveManager::persist_map_entry(const std::string& map_id,
                                    nlohmann::json payload,
                                    DevSaveCoordinator::Priority priority,
                                    const std::string& label,
                                    std::function<void()> on_success) {
    if (map_id.empty()) {
        std::cerr << "[SaveManager] Map identifier is empty; cannot persist map entry\n";
        return false;
    }

    if (coordinator_) {
        coordinator_->enqueue_custom(
            DevSaveCoordinator::IntentKind::MapEntry,
            "map:" + map_id,
            [map_id, payload = std::move(payload)](ManifestStore& store) {
                auto guard = store.scoped_guard("SaveManager::persist_map_entry");
                (void)guard;
                return store.update_map_entry(map_id, payload);
            },
            priority,
            label.empty() ? std::string("Map ") + map_id : label,
            std::move(on_success));
        if (priority == DevSaveCoordinator::Priority::Immediate) {
            coordinator_->flush_now(label);
        }
        return true;
    }

    if (!store_) {
        std::cerr << "[SaveManager] Manifest store unavailable; cannot persist map entry\n";
        return false;
    }

    if (!store_->update_map_entry(map_id, payload)) {
        return false;
    }
    if (priority == DevSaveCoordinator::Priority::Immediate) {
        auto guard = store_->scoped_guard("SaveManager::persist_map_entry");
        (void)guard;
        store_->flush();
        if (on_success) {
            on_success();
        }
    }
    return true;
}

}

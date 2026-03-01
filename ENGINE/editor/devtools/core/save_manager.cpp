#include "save_manager.hpp"

#include <algorithm>
#include <iostream>
#include <unordered_set>

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

bool SaveManager::flush_manifest_stage(const std::string& reason) {
    if (!store_ || !coordinator_) {
        return false;
    }
    coordinator_->flush_now(reason.empty() ? "SaveManager manifest stage" : reason);
    return true;
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
        if (a.get().stage == b.get().stage) {
            return a.get().id < b.get().id;
        }
        return static_cast<int>(a.get().stage) < static_cast<int>(b.get().stage);
    });

    std::unordered_set<std::string> manifest_success;
    manifest_success.reserve(dirty.size());

    batch_save_active_ = true;

    // Stage 1: Manifest
    {
        const auto suppression = coordinator_ ? coordinator_->scoped_flush_suppression()
                                              : DevSaveCoordinator::ScopedFlushSuppression{};
        for (const auto& ref : dirty) {
            const Saveable& saveable = ref.get();
            if (saveable.stage != Stage::Manifest) {
                continue;
            }
            if (saveable.save && saveable.save(priority)) {
                manifest_success.insert(saveable.id);
                any_saved = true;
            }
        }
    }

    if (!manifest_success.empty()) {
        if (coordinator_) {
            flush_manifest_stage(reason.empty() ? "SaveManager manifest flush" : reason);
        } else if (store_) {
            store_->flush();
        }
    }

    // Stage 2: Cache (only entries with successful manifest stage)
    {
        const auto suppression = coordinator_ ? coordinator_->scoped_flush_suppression()
                                              : DevSaveCoordinator::ScopedFlushSuppression{};
        for (const auto& ref : dirty) {
            const Saveable& saveable = ref.get();
            if (saveable.stage != Stage::Cache) {
                continue;
            }
            if (manifest_success.find(saveable.id) == manifest_success.end()) {
                continue;
            }
            if (saveable.save && saveable.save(priority)) {
                any_saved = true;
            }
        }
    }

    // Stage 3: Post
    {
        const auto suppression = coordinator_ ? coordinator_->scoped_flush_suppression()
                                              : DevSaveCoordinator::ScopedFlushSuppression{};
        for (const auto& ref : dirty) {
            const Saveable& saveable = ref.get();
            if (saveable.stage != Stage::Post) {
                continue;
            }
            if (saveable.save && saveable.save(priority)) {
                any_saved = true;
            }
        }
    }

    batch_save_active_ = false;

    if (priority == DevSaveCoordinator::Priority::Immediate) {
        if (coordinator_) {
            coordinator_->flush_now(reason.empty() ? "SaveManager immediate completion" : reason);
        } else if (store_) {
            store_->flush();
        }
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

    if (batch_save_active_) {
        if (!store_) {
            std::cerr << "[SaveManager] Manifest store unavailable; cannot persist map entry\n";
            return false;
        }
        if (!store_->update_map_entry(map_id, payload)) {
            return false;
        }
        if (on_success) {
            on_success();
        }
        return true;
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
        auto immediate_guard = store_->scoped_guard("SaveManager::persist_map_entry");
        (void)immediate_guard;
        store_->flush();
    }
    if (on_success) {
        on_success();
    }
    return true;
}

}

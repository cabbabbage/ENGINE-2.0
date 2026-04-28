#include "save_manager.hpp"

#include <algorithm>
#include <iostream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "devtools/core/manifest_store.hpp"

namespace devmode::core {

namespace {
constexpr const char* kMapPersistGuardReason = "SaveManager::persist_map_entry";
constexpr const char* kManifestFlushIntentKey = "save-manager:manifest-flush";
constexpr const char* kManifestFlushLabel = "Manifest flush";

SaveOrchestrator::Reason map_reason_from_label(const std::string& reason) {
    if (reason.find("focus") != std::string::npos) {
        return SaveOrchestrator::Reason::FocusChange;
    }
    if (reason.find("reload") != std::string::npos) {
        return SaveOrchestrator::Reason::HotReload;
    }
    if (reason.find("auto") != std::string::npos) {
        return SaveOrchestrator::Reason::AutoSave;
    }
    return SaveOrchestrator::Reason::StateChange;
}
}

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

void SaveManager::set_save_status_sink(SaveOrchestrator::StatusSink sink) {
    orchestrator_.set_status_sink(std::move(sink));
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

bool SaveManager::request_manifest_flush(DevSaveCoordinator::Priority priority,
                                         const std::string& reason) {
    if (!store_) {
        return false;
    }

    if (priority == DevSaveCoordinator::Priority::Immediate) {
        // Immediate flushes happen at one explicit exit point in save_dirty().
        return true;
    }

    if (coordinator_) {
        coordinator_->enqueue_custom(
            DevSaveCoordinator::IntentKind::Custom,
            kManifestFlushIntentKey,
            [](ManifestStore& store) {
                // Count as a write only when there is dirty manifest state to flush.
                return store.dirty();
            },
            DevSaveCoordinator::Priority::Debounced,
            reason.empty() ? kManifestFlushLabel : reason);
        return true;
    }

    store_->flush();
    return true;
}

bool SaveManager::save_dirty_with_reason(DevSaveCoordinator::Priority priority,
                                     SaveOrchestrator::Reason reason,
                                     const std::string& label) {
    SaveOrchestrator::Request request;
    request.document_id = "save-manager";
    request.reason = reason;
    request.atomic_write = [this, priority, label]() {
        return this->save_dirty(priority, label);
    };
    auto result = orchestrator_.save(request);
    return result.success;
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
    struct BatchResetGuard {
        bool& active;
        ~BatchResetGuard() { active = false; }
    } reset_guard{batch_save_active_};

    // Stage 1: Manifest
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

    if (!manifest_success.empty()) {
        request_manifest_flush(priority, reason.empty() ? "SaveManager manifest flush" : reason);
    }

    // Stage 2: Cache (only entries with successful manifest stage)
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

    // Stage 3: Post
    for (const auto& ref : dirty) {
        const Saveable& saveable = ref.get();
        if (saveable.stage != Stage::Post) {
            continue;
        }
        if (saveable.save && saveable.save(priority)) {
            any_saved = true;
        }
    }

    if (priority == DevSaveCoordinator::Priority::Immediate) {
        // Explicit immediate flush path for SaveManager-driven writes.
        if (store_) {
            store_->flush();
        }
        if (coordinator_) {
            coordinator_->flush_now(reason.empty() ? "SaveManager immediate completion" : reason);
        }
    }

    return any_saved;
}

bool SaveManager::persist_map_entry_direct(const std::string& map_id,
                                           nlohmann::json payload,
                                           DevSaveCoordinator::Priority priority,
                                           const std::string& flush_reason,
                                           std::function<void()> on_success) {
    ManifestStore::MapPersistOptions write_options;
    write_options.flush = false;
    write_options.guard_reason = kMapPersistGuardReason;
    if (!store_->persist_map_entry(map_id, payload, write_options)) {
        return false;
    }

    if (on_success) {
        on_success();
    }

    if (batch_save_active_) {
        return true;
    }

    if (priority == DevSaveCoordinator::Priority::Immediate) {
        store_->flush();
        if (coordinator_) {
            coordinator_->flush_now(flush_reason);
        }
    } else {
        request_manifest_flush(priority, flush_reason);
    }

    return true;
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

    if (!store_) {
        std::cerr << "[SaveManager] Manifest store unavailable; cannot persist map entry\n";
        return false;
    }

    const std::string flush_reason = label.empty() ? std::string("Map ") + map_id : label;

    if (batch_save_active_) {
        return persist_map_entry_direct(map_id,
                                        std::move(payload),
                                        priority,
                                        flush_reason,
                                        std::move(on_success));
    }

    SaveOrchestrator::Request request;
    request.document_id = map_id;
    request.reason = map_reason_from_label(flush_reason);
    request.conflict_check = [this, map_id]() {
        return store_ && store_->find_map_entry(map_id) != nullptr && store_->has_pending_write();
    };
    request.disk_available_check = []() {
        return true;
    };
    request.atomic_write = [this, map_id, payload = std::move(payload), priority, flush_reason, on_success]() mutable {
        return persist_map_entry_direct(map_id,
                                        std::move(payload),
                                        priority,
                                        flush_reason,
                                        std::move(on_success));
    };

    auto result = orchestrator_.save(request);
    return result.success;
}

}

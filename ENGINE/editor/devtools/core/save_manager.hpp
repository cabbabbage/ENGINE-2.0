#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "devtools/core/dev_save_coordinator.hpp"

namespace devmode::core {

class ManifestStore;

class SaveManager {
public:
    struct Saveable {
        std::string id;
        std::function<bool()> is_dirty;
        std::function<bool(DevSaveCoordinator::Priority)> save;
    };

    void set_manifest_store(ManifestStore* store);
    void set_save_coordinator(DevSaveCoordinator* coordinator);

    void register_saveable(Saveable saveable);
    void clear_saveables();

    bool has_dirty_saveables() const;
    bool save_dirty(DevSaveCoordinator::Priority priority,
                    const std::string& reason = {});

    bool persist_map_entry(const std::string& map_id,
                           nlohmann::json payload,
                           DevSaveCoordinator::Priority priority,
                           const std::string& label,
                           std::function<void()> on_success = {});

private:
    ManifestStore* store_ = nullptr;
    DevSaveCoordinator* coordinator_ = nullptr;
    std::vector<Saveable> saveables_;
};

}

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/manifest/manifest_loader.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/core/save_manager.hpp"

namespace {

manifest::ManifestData make_manifest_data(const nlohmann::json& raw) {
    manifest::ManifestData data;
    data.raw = raw;
    data.assets = data.raw["assets"];
    data.maps = data.raw["maps"];
    return data;
}

bool wait_for_completion(const std::atomic<bool>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done.load(std::memory_order_acquire);
}

}

TEST_CASE("SaveManager batch save handles nested map persist without deadlock") {
    nlohmann::json manifest_json = {
        {"version", 2},
        {"assets", nlohmann::json::object()},
        {"maps", nlohmann::json::object()},
    };

    int flush_count = 0;
    devmode::core::ManifestStore store(
        std::filesystem::path("test_manifest.json"),
        [&manifest_json]() { return make_manifest_data(manifest_json); },
        [&manifest_json](const std::filesystem::path&, const nlohmann::json& updated, int) {
            manifest_json = updated;
        },
        [&flush_count]() { ++flush_count; },
        2);

    devmode::core::SaveManager manager;
    manager.set_manifest_store(&store);

    const std::string map_id = "forrest";
    bool map_dirty = true;
    int on_success_count = 0;

    manager.register_saveable({
        "map-session",
        [&map_dirty]() { return map_dirty; },
        [&manager, &map_dirty, &on_success_count, map_id](devmode::core::DevSaveCoordinator::Priority priority) {
            nlohmann::json payload = {
                {"schema_version", 1},
                {"rooms_data", nlohmann::json::object({{"spawn_room", nlohmann::json::object()}})},
            };
            return manager.persist_map_entry(
                map_id,
                std::move(payload),
                priority,
                "Map session",
                [&map_dirty, &on_success_count]() {
                    map_dirty = false;
                    ++on_success_count;
                });
        },
        devmode::core::SaveManager::Stage::Manifest,
    });

    std::atomic<bool> done{false};
    bool save_result = false;
    std::thread worker([&]() {
        save_result = manager.save_dirty_with_reason(
            devmode::core::DevSaveCoordinator::Priority::Debounced,
            devmode::core::SaveOrchestrator::Reason::AutoSave,
            "SaveManager reentrancy regression");
        done.store(true, std::memory_order_release);
    });

    const bool finished = wait_for_completion(done, std::chrono::seconds(2));
    if (!finished) {
        CHECK_MESSAGE(false, "save_dirty_with_reason timed out (possible deadlock)");
        worker.detach();
        return;
    }
    worker.join();

    CHECK(save_result);
    CHECK(on_success_count == 1);
    CHECK_FALSE(map_dirty);
    CHECK(flush_count == 1);

    const nlohmann::json* entry = store.find_map_entry(map_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->contains("rooms_data"));
    CHECK((*entry)["rooms_data"].is_object());
}

TEST_CASE("SaveManager nested map persist emits only outer orchestrator status events") {
    nlohmann::json manifest_json = {
        {"version", 2},
        {"assets", nlohmann::json::object()},
        {"maps", nlohmann::json::object()},
    };

    devmode::core::ManifestStore store(
        std::filesystem::path("test_manifest_status.json"),
        [&manifest_json]() { return make_manifest_data(manifest_json); },
        [&manifest_json](const std::filesystem::path&, const nlohmann::json& updated, int) {
            manifest_json = updated;
        },
        []() {},
        2);

    devmode::core::SaveManager manager;
    manager.set_manifest_store(&store);

    std::mutex status_mutex;
    std::vector<devmode::core::SaveOrchestrator::Status> statuses;
    manager.set_save_status_sink([&status_mutex, &statuses](devmode::core::SaveOrchestrator::Status status,
                                                            const std::string&) {
        std::lock_guard<std::mutex> lock(status_mutex);
        statuses.push_back(status);
    });

    const std::string map_id = "forrest";
    bool map_dirty = true;
    manager.register_saveable({
        "map-session",
        [&map_dirty]() { return map_dirty; },
        [&manager, &map_dirty, map_id](devmode::core::DevSaveCoordinator::Priority priority) {
            nlohmann::json payload = {
                {"schema_version", 1},
                {"rooms_data", nlohmann::json::object()},
            };
            return manager.persist_map_entry(
                map_id,
                std::move(payload),
                priority,
                "Map session",
                [&map_dirty]() { map_dirty = false; });
        },
        devmode::core::SaveManager::Stage::Manifest,
    });

    const bool ok = manager.save_dirty_with_reason(
        devmode::core::DevSaveCoordinator::Priority::Debounced,
        devmode::core::SaveOrchestrator::Reason::AutoSave,
        "SaveManager status regression");
    CHECK(ok);

    std::vector<devmode::core::SaveOrchestrator::Status> status_copy;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status_copy = statuses;
    }

    REQUIRE(status_copy.size() == 2);
    CHECK(status_copy[0] == devmode::core::SaveOrchestrator::Status::Saving);
    CHECK(status_copy[1] == devmode::core::SaveOrchestrator::Status::Saved);
}

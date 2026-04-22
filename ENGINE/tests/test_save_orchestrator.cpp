#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "devtools/core/save_orchestrator.hpp"

using devmode::core::SaveOrchestrator;

TEST_CASE("SaveOrchestrator serializes overlapping triggers") {
    SaveOrchestrator orchestrator;
    std::atomic<int> writes{0};

    auto run_save = [&](SaveOrchestrator::Reason reason) {
        SaveOrchestrator::Request req;
        req.document_id = "doc";
        req.reason = reason;
        req.atomic_write = [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++writes;
            return true;
        };
        return orchestrator.save(req).success;
    };

    std::thread autosave([&]() { CHECK(run_save(SaveOrchestrator::Reason::AutoSave)); });
    std::thread focus([&]() { CHECK(run_save(SaveOrchestrator::Reason::FocusChange)); });

    autosave.join();
    focus.join();

    CHECK(writes.load() == 2);
}

TEST_CASE("SaveOrchestrator handles rapid state changes") {
    SaveOrchestrator orchestrator;
    std::atomic<int> writes{0};

    for (int i = 0; i < 20; ++i) {
        SaveOrchestrator::Request req;
        req.document_id = "doc-state";
        req.reason = SaveOrchestrator::Reason::StateChange;
        req.atomic_write = [&]() {
            ++writes;
            return true;
        };
        auto result = orchestrator.save(req);
        CHECK(result.success);
        CHECK(result.version == static_cast<std::uint64_t>(i + 1));
    }

    CHECK(writes.load() == 20);
}

TEST_CASE("SaveOrchestrator formatter timeout remains non-blocking") {
    SaveOrchestrator orchestrator;

    SaveOrchestrator::Request req;
    req.document_id = "doc-format";
    req.reason = SaveOrchestrator::Reason::AutoSave;
    req.formatter_non_blocking = true;
    req.formatter_timeout = std::chrono::milliseconds(1);
    req.formatter_or_lint = [](std::chrono::milliseconds) {
        return false;
    };
    req.atomic_write = []() { return true; };

    const auto result = orchestrator.save(req);
    CHECK(result.success);
    CHECK(result.formatter_failed);
}

TEST_CASE("SaveOrchestrator write failure reports failure") {
    SaveOrchestrator orchestrator;

    SaveOrchestrator::Request req;
    req.document_id = "doc-disk";
    req.reason = SaveOrchestrator::Reason::AutoSave;
    req.atomic_write = []() { return false; };

    const auto result = orchestrator.save(req);
    CHECK_FALSE(result.success);
}

TEST_CASE("SaveOrchestrator checksum mismatch catches partial writes") {
    SaveOrchestrator orchestrator;
    bool flip = false;

    SaveOrchestrator::Request req;
    req.document_id = "doc-checksum";
    req.reason = SaveOrchestrator::Reason::HotReload;
    req.checksum = [&]() {
        return flip ? std::size_t{2} : std::size_t{1};
    };
    req.atomic_write = [&]() {
        flip = true;
        return true;
    };

    const auto result = orchestrator.save(req);
    CHECK_FALSE(result.success);
    CHECK(result.verification_failed);
}

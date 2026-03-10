#include "dev_save_coordinator.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "devtools/core/manifest_store.hpp"

namespace devmode::core {

namespace {
std::string summarize_labels(const std::vector<std::string>& labels) {
    if (labels.empty()) {
        return {};
    }
    std::vector<std::string> unique = labels;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() == 1) {
        return unique.front();
    }
    if (unique.size() == 2) {
        return unique.front() + " & " + unique.back();
    }
    std::ostringstream oss;
    const std::size_t max_list = 3;
    for (std::size_t i = 0; i < unique.size() && i < max_list; ++i) {
        if (i > 0) oss << ", ";
        oss << unique[i];
    }
    if (unique.size() > max_list) {
        oss << ", +" << (unique.size() - max_list);
    }
    return oss.str();
}
}

DevSaveCoordinator::DevSaveCoordinator() = default;


DevSaveCoordinator::ScopedFlushSuppression::ScopedFlushSuppression(ScopedFlushSuppression&& other) noexcept {
    coordinator_ = other.coordinator_;
    active_ = other.active_;
    other.coordinator_ = nullptr;
    other.active_ = false;
}

DevSaveCoordinator::ScopedFlushSuppression& DevSaveCoordinator::ScopedFlushSuppression::operator=(ScopedFlushSuppression&& other) noexcept {
    if (this != &other) {
        release();
        coordinator_ = other.coordinator_;
        active_ = other.active_;
        other.coordinator_ = nullptr;
        other.active_ = false;
    }
    return *this;
}

DevSaveCoordinator::ScopedFlushSuppression::~ScopedFlushSuppression() {
    release();
}

void DevSaveCoordinator::ScopedFlushSuppression::release() {
    if (!active_ || !coordinator_) {
        return;
    }
    if (coordinator_->flush_suppression_depth_ > 0) {
        --coordinator_->flush_suppression_depth_;
    }
    coordinator_ = nullptr;
    active_ = false;
}

void DevSaveCoordinator::set_manifest_store(ManifestStore* store) {
    store_ = store;
}

void DevSaveCoordinator::set_notice_sink(NoticeSink sink) {
    notice_sink_ = std::move(sink);
}

void DevSaveCoordinator::begin_frame() {
    telemetry_.writes_this_frame = 0;
}

DevSaveCoordinator::Clock::time_point DevSaveCoordinator::now() const {
    return Clock::now();
}

void DevSaveCoordinator::schedule_deadline(const PendingIntent& intent) {
    if (intent.priority == Priority::Immediate) {
        next_deadline_ = intent.deadline;
        return;
    }
    if (!next_deadline_ || intent.deadline < *next_deadline_) {
        next_deadline_ = intent.deadline;
    }
}

void DevSaveCoordinator::enqueue_manifest_asset(const std::string& asset_key,
                                                nlohmann::json payload,
                                                Priority priority,
                                                const std::string& label,
                                                std::function<void()> on_success) {
    if (asset_key.empty()) {
        return;
    }
    enqueue_custom(IntentKind::ManifestAsset,
                   "asset:" + asset_key,
                   [asset_key, payload = std::move(payload)](ManifestStore& store) {
                       auto session = store.begin_asset_edit(asset_key, true);
                       if (!session) {
                           return false;
                       }
                       session.data() = payload;
                       return session.commit();
                   },
                   priority,
                   label.empty() ? std::string("Asset ") + asset_key : label,
                   std::move(on_success));
}

void DevSaveCoordinator::enqueue_map_entry(const std::string& map_id,
                                           nlohmann::json payload,
                                           Priority priority,
                                           const std::string& label,
                                           std::function<void()> on_success) {
    if (map_id.empty()) {
        return;
    }
    enqueue_custom(IntentKind::MapEntry,
                   "map:" + map_id,
                   [map_id, payload = std::move(payload)](ManifestStore& store) {
                       auto guard = store.scoped_guard("DevSaveCoordinator::enqueue_map_entry");
                       (void)guard;
                       return store.update_map_entry(map_id, payload);
                   },
                   priority,
                   label.empty() ? std::string("Map ") + map_id : label,
                   std::move(on_success));
}

void DevSaveCoordinator::enqueue_custom(IntentKind kind,
                                        const std::string& key,
                                        std::function<bool(ManifestStore&)> apply,
                                        Priority priority,
                                        const std::string& label,
                                        std::function<void()> on_success) {
    if (!apply || key.empty()) {
        return;
    }

    PendingIntent intent;
    intent.kind = kind;
    intent.key = key;
    intent.label = label;
    intent.priority = priority;
    intent.apply = std::move(apply);
    intent.on_success = std::move(on_success);
    intent.deadline = now() + (priority == Priority::Immediate ? std::chrono::milliseconds(0) : debounce_);

    ++telemetry_.total_intents;

    auto it = index_.find(key);
    if (it != index_.end()) {
        intents_[it->second] = std::move(intent);
        ++telemetry_.coalesced_updates;
    } else {
        intents_.push_back(std::move(intent));
        index_[key] = intents_.size() - 1;
    }

    schedule_deadline(intents_[index_[key]]);

    if (priority == Priority::Immediate && flush_suppression_depth_ == 0) {
        flush_now(label);
    }
}

void DevSaveCoordinator::tick() {
    if (!next_deadline_) {
        return;
    }
    const auto current = now();
    if (current < *next_deadline_) {
        return;
    }
    process(false, {});
}

void DevSaveCoordinator::flush_now(const std::string& reason) {
    if (flush_suppression_depth_ > 0) {
        return;
    }
    process(true, reason);
}

DevSaveCoordinator::ScopedFlushSuppression DevSaveCoordinator::scoped_flush_suppression() {
    ++flush_suppression_depth_;
    return ScopedFlushSuppression(this, true);
}

bool DevSaveCoordinator::process(bool force, const std::string& reason) {
    if (processing_) {
        return false;
    }
    if (intents_.empty()) {
        next_deadline_.reset();
        return false;
    }
    if (!store_) {
        intents_.clear();
        index_.clear();
        next_deadline_.reset();
        return false;
    }

    processing_ = true;
    struct ProcessingResetGuard {
        bool& flag;
        ~ProcessingResetGuard() { flag = false; }
    } reset_guard{processing_};

    std::vector<PendingIntent> batch = std::move(intents_);
    intents_.clear();
    index_.clear();
    next_deadline_.reset();

    std::size_t writes = 0;
    bool success = true;
    std::vector<std::string> labels;
    labels.reserve(batch.size());
    bool notify = force;

    for (auto& intent : batch) {
        if (!force && intent.priority == Priority::Debounced) {
            // If we were invoked by a different intent's immediate flush, keep this one queued.
            if (intent.deadline > now()) {
                enqueue_custom(intent.kind,
                               intent.key,
                               std::move(intent.apply),
                               intent.priority,
                               intent.label,
                               std::move(intent.on_success));
                continue;
            }
        }

        notify = notify || intent.priority == Priority::Immediate;
        bool applied = false;
        if (intent.apply) {
            try {
                applied = intent.apply(*store_);
            } catch (const std::exception& ex) {
                std::cerr << "[DevSaveCoordinator] Save intent '" << intent.key
                          << "' threw exception: " << ex.what() << "\n";
                applied = false;
                success = false;
            } catch (...) {
                std::cerr << "[DevSaveCoordinator] Save intent '" << intent.key
                          << "' threw unknown exception\n";
                applied = false;
                success = false;
            }
        }
        success = success && applied;
        if (applied) {
            ++writes;
            ++telemetry_.writes_this_frame;
            if (intent.on_success) {
                try {
                    intent.on_success();
                } catch (const std::exception& ex) {
                    std::cerr << "[DevSaveCoordinator] on_success for intent '" << intent.key
                              << "' threw exception: " << ex.what() << "\n";
                    success = false;
                } catch (...) {
                    std::cerr << "[DevSaveCoordinator] on_success for intent '" << intent.key
                              << "' threw unknown exception\n";
                    success = false;
                }
            }
        }
        if (!intent.label.empty()) {
            labels.push_back(intent.label);
        }
    }

    if (writes > 0) {
        try {
            store_->flush();
            ++telemetry_.flush_count;
        } catch (const std::exception& ex) {
            std::cerr << "[DevSaveCoordinator] Manifest flush threw exception: "
                      << ex.what() << "\n";
            success = false;
        } catch (...) {
            std::cerr << "[DevSaveCoordinator] Manifest flush threw unknown exception\n";
            success = false;
        }
    }

    if (notify) {
        try {
            publish_notice(success && writes > 0, writes, labels, reason);
        } catch (const std::exception& ex) {
            std::cerr << "[DevSaveCoordinator] publish_notice threw exception: "
                      << ex.what() << "\n";
        } catch (...) {
            std::cerr << "[DevSaveCoordinator] publish_notice threw unknown exception\n";
        }
    }

    return success;
}

void DevSaveCoordinator::publish_notice(bool success,
                                        std::size_t writes,
                                        const std::vector<std::string>& labels,
                                        const std::string& reason) {
    if (!notice_sink_) {
        return;
    }
    std::string summary = summarize_labels(labels);
    std::ostringstream oss;
    if (success) {
        if (writes > 0) {
            oss << "Saved " << writes << (writes == 1 ? " change" : " changes");
        } else {
            oss << "No changes to save";
        }
    } else {
        oss << "Save failed";
        if (writes > 0) {
            oss << " (" << writes << " change" << (writes == 1 ? "" : "s") << ")";
        }
    }
    if (!summary.empty()) {
        oss << ": " << summary;
    }
    if (!reason.empty()) {
        oss << " [" << reason << "]";
    }
    notice_sink_(success, oss.str());
}

}

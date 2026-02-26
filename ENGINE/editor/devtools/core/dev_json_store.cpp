#include "devtools/core/dev_json_store.hpp"

#include <SDL3/SDL_log.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <optional>
#include <utility>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdio>
#ifdef _WIN32
#  include <share.h>
#  include <io.h>
#  include <fcntl.h>
#  include <windows.h>
#endif

namespace devmode::core {
namespace {
constexpr std::chrono::milliseconds kDefaultDebounce{400};

struct PathHash {
    std::size_t operator()(const std::filesystem::path& p) const noexcept {
        return std::filesystem::hash_value(p);
    }
};

struct DigestEntry {
    std::filesystem::file_time_type mtime{};
    std::size_t hash{};
    nlohmann::json data = nlohmann::json::object();
    bool valid = false;
};

struct PendingWrite {
    std::filesystem::path path;
    nlohmann::json data;
    std::string serialized;
    std::size_t hash{};
    std::chrono::steady_clock::time_point deadline;
};

void log_error(const std::string& message) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message.c_str());
}

void log_info(const std::string& message) {
    SDL_Log("%s", message.c_str());
}

bool write_file(const std::filesystem::path& path,
                const std::string& payload,
                std::ostream& error_sink) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error_sink << "[DevJsonStore] Failed to create parent directory for '" << path.string() << "': " << ec.message() << "\n";
            return false;
        }
    }

    const std::filesystem::path tmp_path = path;
    const std::filesystem::path tmp_full = tmp_path.string() + ".tmp";

    std::filesystem::perms target_perms = std::filesystem::perms::unknown;
    const bool target_exists = std::filesystem::exists(path, ec);
    if (!ec && target_exists) {
        target_perms = std::filesystem::status(path, ec).permissions();
        ec.clear();
    } else {
        ec.clear();
    }

#ifdef _WIN32

    std::wstring wtmp = tmp_full.wstring();
    FILE* fp = _wfsopen(wtmp.c_str(), L"wb", _SH_DENYRW);
    if (!fp) {
        error_sink << "[DevJsonStore] Failed to open temp file '" << tmp_full.string() << "' for writing\n";
        return false;
    }
    const size_t written = fwrite(payload.data(), 1, payload.size(), fp);
    if (written != payload.size()) {
        error_sink << "[DevJsonStore] Short write to temp file '" << tmp_full.string() << "'\n";
        fclose(fp);
        std::filesystem::remove(tmp_full, ec);
        return false;
    }
    fflush(fp);

    _commit(_fileno(fp));
    fclose(fp);

    if (target_perms != std::filesystem::perms::unknown) {
        std::filesystem::permissions(tmp_full, target_perms, ec);
        ec.clear();
    }

    std::wstring wdst = path.wstring();
    if (!MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        error_sink << "[DevJsonStore] MoveFileExW failed replacing '" << path.string() << "' with temp: error " << err << "\n";

        std::filesystem::remove(tmp_full, ec);
        return false;
    }

    return true;
#else

    {
        std::ofstream out(tmp_full, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error_sink << "[DevJsonStore] Failed to open temp file '" << tmp_full << "' for writing\n";
            return false;
        }
        out << payload;
        out.flush();
        if (!out.good()) {
            error_sink << "[DevJsonStore] Stream error while writing temp '" << tmp_full << "'\n";
            return false;
        }
    }

    if (target_perms != std::filesystem::perms::unknown) {
        std::filesystem::permissions(tmp_full, target_perms, ec);
        ec.clear();
    }

    std::filesystem::rename(tmp_full, path, ec);
    if (ec) {
        error_sink << "[DevJsonStore] rename('" << tmp_full.string() << "' -> '" << path.string() << "') failed: " << ec.message() << "\n";

        std::filesystem::remove(tmp_full, ec);
        return false;
    }
    return true;
#endif
}

std::optional<DigestEntry> read_existing_digest(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }

    DigestEntry entry;
    entry.mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    entry.hash = std::hash<std::string>{}(contents);
    try {
        entry.data = nlohmann::json::parse(contents);
        if (!entry.data.is_object()) {
            entry.data = nlohmann::json::object();
        }
    } catch (...) {
        entry.data = nlohmann::json::object();
    }
    entry.valid = true;
    return entry;
}

}

struct DevJsonStore::Impl {
    Impl()
#ifndef DEV_MODE_DISABLE_JSON_DEBOUNCE
        : worker_([this]() { this->worker_loop(); })
#endif
    {}

    ~Impl() {
        shutdown();
    }

    bool has_pending_write(const std::filesystem::path& path) const {
#ifdef DEV_MODE_DISABLE_JSON_DEBOUNCE
        (void)path;
        return false;
#else
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_writes_.find(path) != pending_writes_.end();
#endif
    }

    nlohmann::json load(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (ec) {
                log_error("[DevJsonStore] exists(" + path.string() + ") failed: " + ec.message());
            }
            return nlohmann::json::object();
        }

        auto file_time = std::filesystem::last_write_time(path, ec);
        if (ec) {
            log_error("[DevJsonStore] last_write_time(" + path.string() + ") failed: " + ec.message());
            return nlohmann::json::object();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = digest_cache_.find(path);
            if (it != digest_cache_.end() && it->second.valid && it->second.mtime == file_time) {
                return it->second.data;
            }
        }

        std::ifstream in(path);
        if (!in.is_open()) {
            log_error("[DevJsonStore] Failed to open '" + path.string() + "' for reading");
            return nlohmann::json::object();
        }
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t hash = std::hash<std::string>{}(contents);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = digest_cache_.find(path);
            if (it != digest_cache_.end() && it->second.valid && it->second.mtime == file_time && it->second.hash == hash) {
                return it->second.data;
            }
        }

        nlohmann::json parsed = nlohmann::json::object();
        try {
            parsed = nlohmann::json::parse(contents);
            if (!parsed.is_object()) {
                parsed = nlohmann::json::object();
            }
        } catch (...) {
            parsed = nlohmann::json::object();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            digest_cache_[path] = DigestEntry{file_time, hash, parsed, true};
        }
        return parsed;
    }

    void submit(const std::filesystem::path& path,
                const nlohmann::json& data,
                int indent) {
#ifdef DEV_MODE_DISABLE_JSON_DEBOUNCE
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = digest_cache_.find(path);
            if (it != digest_cache_.end() && it->second.valid && it->second.data == data) {
                return;
            }
        }
        std::ostringstream errors;
        std::string payload = data.dump(indent);
        const std::size_t payload_hash = std::hash<std::string>{}(payload);

        if (auto disk = read_existing_digest(path)) {
            if (disk->valid && disk->hash == payload_hash && disk->data == data) {
                std::lock_guard<std::mutex> lock(mutex_);
                digest_cache_[path] = *disk;
                return;
            }
        }

        if (!write_file(path, payload, errors)) {
            log_error(errors.str());
            return;
        }
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            mtime = std::filesystem::file_time_type::clock::now();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            digest_cache_[path] = DigestEntry{mtime, payload_hash, data, true};
        }
#else
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cache_it = digest_cache_.find(path);
            if (cache_it != digest_cache_.end() && cache_it->second.valid && cache_it->second.data == data) {
                return;
            }
            auto pending_it = pending_writes_.find(path);
            if (pending_it != pending_writes_.end() && pending_it->second.data == data) {
                pending_it->second.deadline = now + kDefaultDebounce;
                cv_.notify_one();
                return;
            }
        }

        PendingWrite pending;
        pending.path = path;
        pending.data = data;
        pending.serialized = data.dump(indent);
        pending.hash = std::hash<std::string>{}(pending.serialized);
        pending.deadline = now + kDefaultDebounce;

        bool should_probe_disk = false;
        std::filesystem::file_time_type cached_mtime{};
        bool have_cached_mtime = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cache_it = digest_cache_.find(path);
            if (cache_it != digest_cache_.end() && cache_it->second.valid) {
                if (cache_it->second.hash == pending.hash && cache_it->second.data == data) {
                    return;
                }
                cached_mtime = cache_it->second.mtime;
                have_cached_mtime = true;
            } else {
                should_probe_disk = true;
            }

            auto pending_it = pending_writes_.find(path);
            if (pending_it != pending_writes_.end() && pending_it->second.hash == pending.hash && pending_it->second.data == data) {
                pending_it->second.deadline = pending.deadline;
                cv_.notify_one();
                return;
            }
        }

        if (!should_probe_disk) {
            std::error_code mtime_error;
            auto disk_mtime = std::filesystem::last_write_time(path, mtime_error);
            if (!mtime_error) {
                if (!have_cached_mtime || disk_mtime != cached_mtime) {
                    should_probe_disk = true;
                }
            }
        }

        if (should_probe_disk) {
            if (auto disk = read_existing_digest(path)) {
                if (disk->valid && disk->hash == pending.hash && disk->data == data) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    digest_cache_[path] = *disk;
                    return;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_writes_.find(path);
            if (it != pending_writes_.end()) {
                it->second.data = pending.data;
                it->second.serialized = pending.serialized;
                it->second.hash = pending.hash;
                it->second.deadline = pending.deadline;
            } else {
                pending_writes_.emplace(path, std::move(pending));
            }
        }
        cv_.notify_one();
#endif
    }

    void flush_all() {
#ifdef DEV_MODE_DISABLE_JSON_DEBOUNCE

        return;
#else
        std::vector<PendingWrite> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [path, pending] : pending_writes_) {
                ready.push_back(std::move(pending));
            }
            pending_writes_.clear();
        }
        flush_ready(std::move(ready));
#endif
    }

    void shutdown() {
#ifdef DEV_MODE_DISABLE_JSON_DEBOUNCE
        return;
#else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            stopped_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
#endif
    }

#ifndef DEV_MODE_DISABLE_JSON_DEBOUNCE
    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (stopped_) {
                break;
            }

            if (pending_writes_.empty()) {
                cv_.wait(lock, [this]() { return stopped_ || !pending_writes_.empty(); });
                continue;
            }

            auto next_deadline = std::chrono::steady_clock::time_point::max();
            auto now = std::chrono::steady_clock::now();
            for (const auto& [path, pending] : pending_writes_) {
                if (pending.deadline < next_deadline) {
                    next_deadline = pending.deadline;
                }
            }

            if (cv_.wait_until(lock, next_deadline, [this]() { return stopped_ || pending_writes_.empty(); })) {
                continue;
            }

            now = std::chrono::steady_clock::now();
            std::vector<PendingWrite> ready;
            for (auto it = pending_writes_.begin(); it != pending_writes_.end();) {
                if (it->second.deadline <= now) {
                    ready.push_back(std::move(it->second));
                    it = pending_writes_.erase(it);
                } else {
                    ++it;
                }
            }

            if (ready.empty()) {
                continue;
            }

            lock.unlock();
            flush_ready(std::move(ready));
            lock.lock();
        }

        std::vector<PendingWrite> remaining;
        for (auto& [path, pending] : pending_writes_) {
            remaining.push_back(std::move(pending));
        }
        pending_writes_.clear();
        lock.unlock();
        flush_ready(std::move(remaining));
    }

    void flush_ready(std::vector<PendingWrite>&& writes) {
        if (writes.empty()) {
            return;
        }

        struct Result {
            std::filesystem::path path;
            nlohmann::json data;
            std::size_t hash{};
            std::filesystem::file_time_type mtime{};
            bool success = false;
};

        std::vector<Result> results;
        results.reserve(writes.size());

        std::ostringstream error_buffer;
        for (auto& pending : writes) {
            Result result;
            result.path = pending.path;
            result.data = pending.data;
            result.hash = pending.hash;

            std::ostringstream local_errors;
            if (write_file(pending.path, pending.serialized, local_errors)) {
                std::error_code ec;
                result.mtime = std::filesystem::last_write_time(pending.path, ec);
                if (ec) {
                    result.mtime = std::filesystem::file_time_type::clock::now();
                    error_buffer << "[DevJsonStore] last_write_time('" << pending.path.string() << "') failed after write: " << ec.message() << "\n";
                }
                result.success = true;
            } else {
                error_buffer << local_errors.str();
            }

            results.push_back(std::move(result));
        }

        if (!error_buffer.str().empty()) {
            log_error(error_buffer.str());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& result : results) {
                if (!result.success) {
                    continue;
                }
                digest_cache_[result.path] = DigestEntry{result.mtime, result.hash, result.data, true};
            }
        }
    }
#endif

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::filesystem::path, DigestEntry, PathHash> digest_cache_;
#ifndef DEV_MODE_DISABLE_JSON_DEBOUNCE
    std::unordered_map<std::filesystem::path, PendingWrite, PathHash> pending_writes_;
    std::thread worker_;
    bool stopped_ = false;
#endif
};

DevJsonStore& DevJsonStore::instance() {
    static DevJsonStore store;
    return store;
}

DevJsonStore::DevJsonStore()
    : impl_(new Impl()) {}

DevJsonStore::~DevJsonStore() {
    delete impl_;
}

nlohmann::json DevJsonStore::load(const std::filesystem::path& path) {
    return impl_->load(path);
}

void DevJsonStore::submit(const std::filesystem::path& path,
                          const nlohmann::json& data,
                          int indent) {
    impl_->submit(path, data, indent);
}

void DevJsonStore::flush_all() {
    impl_->flush_all();
}

bool DevJsonStore::has_pending_write(const std::filesystem::path& path) const {
    return impl_->has_pending_write(path);
}

void DevJsonStore::shutdown() {
    impl_->shutdown();
}

}

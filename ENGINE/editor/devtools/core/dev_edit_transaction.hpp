#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace devmode::core {

class DevEditTransaction {
public:
    struct Hooks {
        std::function<bool()> mutate;
        std::function<bool(const nlohmann::json&)> validate;
        std::function<bool()> commit;
        std::function<void()> on_commit_success;
        std::function<void()> on_rollback;
    };

    DevEditTransaction(nlohmann::json& target,
                       std::uint64_t& edit_version,
                       std::string label = {});

    bool run(const Hooks& hooks);

    const std::string& error_message() const { return error_message_; }
    std::uint64_t pre_edit_version() const { return pre_edit_version_; }

private:
    void rollback(const Hooks& hooks, const std::string& message);

    nlohmann::json& target_;
    std::uint64_t& edit_version_;
    std::uint64_t pre_edit_version_ = 0;
    nlohmann::json snapshot_{};
    std::string label_{};
    std::string error_message_{};
};

} // namespace devmode::core

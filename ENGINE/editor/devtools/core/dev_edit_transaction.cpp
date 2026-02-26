#include "dev_edit_transaction.hpp"

#include <utility>

namespace devmode::core {

DevEditTransaction::DevEditTransaction(nlohmann::json& target,
                                       std::uint64_t& edit_version,
                                       std::string label)
    : target_(target),
      edit_version_(edit_version),
      label_(std::move(label)) {}

bool DevEditTransaction::run(const Hooks& hooks) {
    error_message_.clear();
    pre_edit_version_ = edit_version_;
    snapshot_ = target_;

    if (!hooks.mutate || !hooks.mutate()) {
        rollback(hooks, "mutation failed");
        return false;
    }

    if (hooks.validate && !hooks.validate(target_)) {
        rollback(hooks, "validation failed");
        return false;
    }

    if (!hooks.commit || !hooks.commit()) {
        rollback(hooks, "commit failed");
        return false;
    }

    ++edit_version_;
    if (hooks.on_commit_success) {
        hooks.on_commit_success();
    }
    return true;
}

void DevEditTransaction::rollback(const Hooks& hooks, const std::string& message) {
    target_ = snapshot_;
    edit_version_ = pre_edit_version_;
    if (!label_.empty()) {
        error_message_ = label_ + ": " + message;
    } else {
        error_message_ = message;
    }
    if (hooks.on_rollback) {
        hooks.on_rollback();
    }
}

} // namespace devmode::core

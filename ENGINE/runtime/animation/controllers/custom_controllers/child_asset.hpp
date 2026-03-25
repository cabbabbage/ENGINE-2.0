#pragma once

#include <optional>
#include <string>

#include "assets/asset/anchor_point.hpp"

class Asset;
class Assets;

class ChildAsset {
public:
    explicit ChildAsset(std::string asset_name);
    ~ChildAsset();

    ChildAsset(const ChildAsset&) = delete;
    ChildAsset& operator=(const ChildAsset&) = delete;
    ChildAsset(ChildAsset&& other) noexcept;
    ChildAsset& operator=(ChildAsset&& other) noexcept;

    void destroy();
    void hide();
    void unhide();
    void set_grid_point(const std::string& parent_anchor_name);
    void set_grid_point(const AnchorPoint& parent_anchor);
    void bind(const std::string& parent_anchor_name);
    void bind(const AnchorPoint& parent_anchor);
    void unbind();
    bool is_hidden() const;
    bool is_bound() const;
    Asset* get_asset() const;
    void update();
    static void sync_registered_for_owner(Asset* owner);

private:
    bool ensure_child_alive();
    bool apply_anchor_solution(const AnchorPoint& parent_anchor);
    bool place_once(const AnchorPoint& parent_anchor, bool keep_bound);
    bool set_child_hidden_state(bool hidden);
    std::optional<AnchorPoint> resolve_owner_anchor(const std::string& anchor_name) const;
    void refresh_hidden_state();
    void move_from(ChildAsset&& other) noexcept;
    void register_with_owner();
    void unregister_from_owner();

    std::string asset_name_;
    Asset* owner_ = nullptr;
    Assets* assets_ = nullptr;
    Asset* child_ = nullptr;
    std::string bound_anchor_name_;
    bool bound_ = false;
    bool manual_hidden_ = false;
    bool auto_hidden_for_anchor_ = true;
    bool has_successful_sync_ = false;
};

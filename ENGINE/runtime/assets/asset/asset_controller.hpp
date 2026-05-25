#ifndef ASSET_ASSET_CONTROLLER_HPP
#define ASSET_ASSET_CONTROLLER_HPP

#include <optional>

class Asset;
class Input;

struct OrphanImpulse {
    float direction_x = 0.0f;
    float direction_z = 0.0f;
    float force = 0.0f;
    float upward_force = 0.0f;
};

class AssetController {

	public:
    AssetController();
    virtual ~AssetController();
    virtual void update(const Input& in) = 0;
    virtual void process_pending_attacks(Asset& self) = 0;
    virtual bool requires_runtime_update() const { return true; }
    virtual void on_pre_delete(Asset& self) { (void)self; }
    virtual void on_orphaned(Asset& self,
                             Asset* former_parent,
                             std::optional<OrphanImpulse> impulse = std::nullopt) {
        (void)self;
        (void)former_parent;
        (void)impulse;
    }
    virtual void on_interact(Asset& self, Asset* instigator) {
        (void)self;
        (void)instigator;
    }
};

#endif

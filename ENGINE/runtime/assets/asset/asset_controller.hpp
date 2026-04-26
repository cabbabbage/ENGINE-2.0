#ifndef ASSET_ASSET_CONTROLLER_HPP
#define ASSET_ASSET_CONTROLLER_HPP

class Asset;
class Input;

class AssetController {

	public:
    AssetController();
    virtual ~AssetController();
    virtual void update(const Input& in) = 0;
    virtual void process_pending_attacks(Asset& self) = 0;
    virtual void on_pre_delete(Asset& self) { (void)self; }
    virtual void on_orphaned(Asset& self, Asset* former_parent) {
        (void)self;
        (void)former_parent;
    }
};

#endif

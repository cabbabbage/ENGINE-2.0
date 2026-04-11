#pragma once

class Asset;
class Assets;

namespace animation_update::custom_controllers {

struct ControllerGameContext;

Asset* resolve_valid_player_target(const ControllerGameContext& context);
Asset* resolve_valid_player_target(Asset* self, Assets* assets);
void dispatch_contact_attack(const ControllerGameContext& context);
void dispatch_contact_attack(Asset* self, Asset* player);

} // namespace animation_update::custom_controllers

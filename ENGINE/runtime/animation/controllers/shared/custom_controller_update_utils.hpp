#pragma once

class Asset;
class Assets;

namespace animation_update::custom_controllers {

Asset* resolve_valid_player_target(Asset* self, Assets* assets);
void dispatch_contact_attack(Asset* self, Asset* player);

} // namespace animation_update::custom_controllers

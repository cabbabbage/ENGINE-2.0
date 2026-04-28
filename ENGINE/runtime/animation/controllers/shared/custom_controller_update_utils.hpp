#pragma once

class Asset;
class Assets;

namespace animation_update::custom_controllers {

struct ControllerGameContext;

Asset* resolve_valid_player_target(const ControllerGameContext& context);
Asset* resolve_valid_player_target(Asset* self, Assets* assets);
void dispatch_contact_attack(const ControllerGameContext& context);
void dispatch_contact_attack(Asset* self, Asset* player);
void dispatch_interact(const ControllerGameContext& context, Asset* target);
void dispatch_interact(Asset* instigator, Asset* target);
void begin_reverse_current_animation_until_stop(const ControllerGameContext& context);
void begin_reverse_current_animation_until_stop(Asset* self);
void begin_reverse_current_animation_to_default(const ControllerGameContext& context);
void begin_reverse_current_animation_to_default(Asset* self);
void stop_reverse_current_animation(const ControllerGameContext& context);
void stop_reverse_current_animation(Asset* self);

} // namespace animation_update::custom_controllers

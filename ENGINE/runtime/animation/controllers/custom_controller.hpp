#pragma once

#include "animation/controllers/shared/custom_controller_base.hpp"
#include "animation/controllers/shared/controller_types.hpp"

namespace animation_update::custom_controllers {
using PatrolState = internal::PatrolState;
using BehaviorState = internal::BehaviorState;
using EnemyBehaviorConfig = EnemyAgentConfig;
}

namespace custom_controller_api = animation_update::custom_controllers;

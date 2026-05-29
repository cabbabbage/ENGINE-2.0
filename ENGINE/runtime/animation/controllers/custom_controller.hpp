#pragma once

#include "animation/controllers/shared/custom_controller_base.hpp"

namespace animation_update::custom_controllers {
using MovementConfig = internal::MovementConfig;
using PatrolState = internal::PatrolState;
using BehaviorMode = internal::BehaviorMode;
using BehaviorState = internal::BehaviorState;
using EnemyBehaviorConfig = internal::EnemyBehaviorConfig;
}

namespace custom_controller_api = animation_update::custom_controllers;

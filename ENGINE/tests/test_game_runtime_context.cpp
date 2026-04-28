#include <doctest/doctest.h>

#include "core/game_runtime_context.hpp"

namespace runtime_ctx = runtime::context;

TEST_CASE("GameRuntimeContext room fly aggression decays after timeout") {
    runtime_ctx::GameRuntimeContext context;

    context.begin_frame(nullptr, 1u, 0.0f, nullptr, nullptr, nullptr, nullptr);
    context.set_room_fly_aggression("room_a", 20.0f);
    CHECK(context.is_room_fly_aggressive("room_a"));

    context.begin_frame(nullptr, 2u, 19.5f, nullptr, nullptr, nullptr, nullptr);
    CHECK(context.is_room_fly_aggressive("room_a"));

    context.begin_frame(nullptr, 3u, 0.6f, nullptr, nullptr, nullptr, nullptr);
    CHECK_FALSE(context.is_room_fly_aggressive("room_a"));
}

TEST_CASE("GameRuntimeContext room fly aggression retrigger extends expiry") {
    runtime_ctx::GameRuntimeContext context;

    context.begin_frame(nullptr, 1u, 0.0f, nullptr, nullptr, nullptr, nullptr);
    context.set_room_fly_aggression("room_b", 20.0f);

    context.begin_frame(nullptr, 2u, 15.0f, nullptr, nullptr, nullptr, nullptr);
    CHECK(context.is_room_fly_aggressive("room_b"));

    context.set_room_fly_aggression("room_b", 20.0f);
    context.begin_frame(nullptr, 3u, 10.0f, nullptr, nullptr, nullptr, nullptr);
    CHECK(context.is_room_fly_aggressive("room_b"));

    context.begin_frame(nullptr, 4u, 10.1f, nullptr, nullptr, nullptr, nullptr);
    CHECK_FALSE(context.is_room_fly_aggressive("room_b"));
}

TEST_CASE("GameRuntimeContext begin_frame updates frame/player snapshot without resetting prior room aggression") {
    runtime_ctx::GameRuntimeContext context;

    context.begin_frame(nullptr, 10u, 0.0f, nullptr, nullptr, nullptr, nullptr);
    context.set_room_fly_aggression("room_c", 2.0f);
    CHECK(context.is_room_fly_aggressive("room_c"));

    context.begin_frame(nullptr, 11u, 0.5f, nullptr, reinterpret_cast<Asset*>(0x1), nullptr, nullptr);
    CHECK(context.frame_id() == 11u);
    CHECK(context.player() == reinterpret_cast<Asset*>(0x1));
    CHECK(context.is_room_fly_aggressive("room_c"));
}

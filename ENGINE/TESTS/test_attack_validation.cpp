#include "doctest/doctest.h"

#include "asset/animation_frame.hpp"
#include "animation/attack_validation.hpp"

using animation_update::AttackValidation;
using animation_update::CombatantSnapshot;
using animation_update::CombatPlane;
using animation_update::GeometryContext;
using animation_update::FrameAttackGeometry;
using animation_update::FrameHitGeometry;
using animation_update::Attack;

TEST_CASE("AttackValidation mirrors attack paths when flipped") {
    FrameAttackGeometry::Vector vec;
    vec.start_x = 0.0f;
    vec.end_x = 10.0f;
    vec.control_x = 5.0f;

    GeometryContext ctx{};
    ctx.anchor = SDL_Point{100, 50};
    ctx.scale = 1.0f;

    const auto path = AttackValidation::attack_vector_path(vec, ctx, 2);
    REQUIRE(path.size() == 3);
    CHECK(path.front().x == doctest::Approx(100.0f));
    CHECK(path.front().y == doctest::Approx(50.0f));
    CHECK(path.back().x == doctest::Approx(110.0f));

    ctx.flipped = true;
    const auto mirrored = AttackValidation::attack_vector_path(vec, ctx, 2);
    REQUIRE(mirrored.size() == 3);
    CHECK(mirrored.back().x == doctest::Approx(90.0f));
}

TEST_CASE("AttackValidation reports a hit when an attack vector touches a hitbox") {
    AnimationFrame attacker_frame;
    attacker_frame.frame_index = 7;
    FrameAttackGeometry::Vector vec;
    vec.start_x = 0.0f;
    vec.end_x = 10.0f;
    vec.damage = 5;
    attacker_frame.attack_geometry.vectors.push_back(vec);

    AnimationFrame target_frame;
    FrameHitGeometry::HitBox hitbox;
    hitbox.half_width = 5.0f;
    hitbox.half_height = 5.0f;
    target_frame.hit_geometry.boxes.push_back(hitbox);

    CombatantSnapshot attacker_snapshot{
        "attacker", "Hero", &attacker_frame, GeometryContext{SDL_Point{0, 0}, 1.0f, false, CombatPlane::XY}
    };
    CombatantSnapshot target_snapshot{
        "dummy", "Target", &target_frame, GeometryContext{SDL_Point{10, 0}, 1.0f, false, CombatPlane::XY}
    };

    auto result = AttackValidation::compute_attack_if_hit(attacker_snapshot, target_snapshot);
    REQUIRE(result.has_value());
    CHECK(result->damage_amount == 5);
    CHECK(result->hit_x == doctest::Approx(5.0f));
    CHECK(result->hit_y == doctest::Approx(0.0f));

    target_snapshot.transform.anchor.x = 30;
    result = AttackValidation::compute_attack_if_hit(attacker_snapshot, target_snapshot);
    CHECK_FALSE(result.has_value());
}

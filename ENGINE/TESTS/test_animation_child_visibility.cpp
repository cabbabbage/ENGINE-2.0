#include "doctest/doctest.h"

#include "animation/child_attachment_controller.hpp"
#include "animation/child_attachment_math.hpp"

TEST_CASE("AnimationChildFrameData defaults to visible when flag is omitted") {
    AnimationChildFrameData child;
    CHECK(child.visible);
}

TEST_CASE("Child rotation mirrors when parent flips horizontally") {
    const float original = 20.0f;
    CHECK(mirrored_child_rotation(false, original) == doctest::Approx(original));
    CHECK(mirrored_child_rotation(true, original) == doctest::Approx(-original));
}

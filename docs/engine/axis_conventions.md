# Axis Conventions

This document defines the canonical world-space convention used everywhere in the engine runtime, rendering pipeline, and Dev Mode tooling.

## Canonical axes

- **X (right):** horizontal movement and offsets.
- **Y (height):** vertical elevation.
- **Z (depth):** forward/back world depth.

`axis::WorldPos` in `ENGINE/runtime/core/axis_convention.hpp` is the canonical coordinate container and follows `x=right`, `y=height`, `z=depth`.

## Required usage rules

1. World-space APIs and data structures must treat **Y** as vertical height.
2. World-space APIs and data structures must treat **Z** as depth.
3. Loading, editing, runtime transforms, rendering, and saving must use direct canonical values with no implicit axis swaps.
4. Serialization keys and editor labels must match canonical semantics and avoid ambiguous axis wording.
5. New helpers must not introduce temporary adapters or compatibility translations for older axis interpretations.

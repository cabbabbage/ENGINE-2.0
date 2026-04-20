# Asset Manifest Contract (Post-Migration)

Each asset manifest entry is authoritative via four booleans:

- `movement_enabled`
- `attack_box_enabled`
- `hitbox_enabled`
- `impassable_box_enabled`
- `floor_boxes_enabled`

All four flags are required on every asset entry.

## Persistence Rules

- Disabled systems omit their payload keys entirely.
- Do not persist placeholder empty structures for disabled systems.
- `movement_enabled=false`:
  - Omit animation `movement`, `movement_paths`, and `movement_total`.
- `hitbox_enabled=false`:
  - Omit animation `hit_boxes`.
- `attack_box_enabled=false`:
  - Omit animation `attack_boxes`.
- `impassable_box_enabled=false`:
  - Omit asset-level `impassable_boxes`.
- `floor_boxes_enabled=false`:
  - Omit asset-level `floor_boxes`.

## Impassable Boxes

`impassable_boxes` is asset-scoped only (never animation/frame scoped).

Canonical impassable box fields:

- `id` (string)
- `type` (`"impassable_box"`)
- `name` (string)
- `enabled` (bool)
- `extrusion_amount` (integer)
- `flatten_bottom_to_floor` (bool)
- `anchor_link` (string)
- `rotation_degrees` (number)
- `position` (object with `x`, `y`)
- `size` (object with `w`, `h`)

## Floor Boxes

`floor_boxes` is asset-scoped only (never animation/frame scoped).

Canonical floor box fields:

- `id` (string)
- `name` (string)
- `position_x` (number)
- `position_z` (number)
- `width` (number)
- `depth` (number)
- `enabled` (bool)
- `tags` (array of strings)

Behavior:

- `floor_boxes` are axis-aligned floor rectangles (no per-box rotation).
- `boundary` is stripped from floor-box tags during normalization and has no runtime behavior.

## Runtime/Loader Behavior

- Loader uses explicit boolean gates; it does not infer enablement from payload presence.
- `movement_enabled=true`: load authored movement only (no synthesized fallback movement payload).
- `hitbox_enabled=false`: skip hitbox parsing and keep runtime hitbox caches empty.
- `attack_box_enabled=false`: skip attack-box parsing and keep runtime attack-box caches empty.
- `impassable_box_enabled=false`: skip impassable-box load into runtime state.
- `floor_boxes_enabled=false`: skip floor-box load into runtime state.

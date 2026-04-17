# Asset Manifest Contract (Post-Migration)

Each asset manifest entry is authoritative via four booleans:

- `movement_enabled`
- `attack_box_enabled`
- `hitbox_enabled`
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
- `floor_boxes_enabled=false`:
  - Omit asset-level `floor_boxes`.

## Floor Boxes

`floor_boxes` is asset-scoped only (never animation/frame scoped).

Canonical floor box fields:

- `id` (string)
- `name` (string)
- `is_boundary` (bool)
- `position_x` (number)
- `position_z` (number)
- `width` (number)
- `depth` (number)
- `rotation_degrees` (number)
- `enabled` (bool)

Invariant:

- At most one `is_boundary=true` floor box per asset.
- Normalization is deterministic: first boundary entry wins, later boundary entries are demoted to `is_boundary=false`.

## Runtime/Loader Behavior

- Loader uses explicit boolean gates; it does not infer enablement from payload presence.
- `movement_enabled=true`: load authored movement only (no synthesized fallback movement payload).
- `hitbox_enabled=false`: skip hitbox parsing and keep runtime hitbox caches empty.
- `attack_box_enabled=false`: skip attack-box parsing and keep runtime attack-box caches empty.
- `floor_boxes_enabled=false`: skip floor-box load into runtime state.

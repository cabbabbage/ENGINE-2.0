# Asset Manifest Contract (Post-Migration)

Each asset manifest entry is authoritative via four booleans:

- `movement_enabled`
- `attack_box_enabled`
- `hitbox_enabled`
- `impassable_enabled`
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
- `impassable_enabled=false`:
  - Omit asset-level `impassable_shapes`.
- `floor_boxes_enabled=false`:
  - Omit asset-level `floor_boxes`.

## Impassable Shapes

`impassable_shapes` is asset-scoped only (never animation/frame scoped).

Canonical impassable shape fields:

- `id` (string)
- `name` (string)
- `enabled` (bool)
- `points` (ordered array of `{x, y}` relative anchor points)

Rules:

- Minimum `points` length is `3`.
- Point order defines polygon edges.
- Persisted point order is canonicalized to a consistent winding.
- Self-intersecting polygons are rejected during normalization/parse.

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
- `candidate` (optional object):
  - `candidates` (candidate array using the shared spawn candidate schema)
  - `grid_resolution` (integer in `[2, 8]`, defaults to `4`)

Behavior:

- `floor_boxes` are axis-aligned floor rectangles (no per-box rotation).
- `boundary` is stripped from floor-box tags during normalization and has no runtime behavior.
- `candidate` is optional. If absent, no floor-box candidate spawning runs for that box.
- Candidate arrays are sanitized to always include a `null` entry; `null` cannot be removed and may be set to chance `0`.

## Runtime/Loader Behavior

- Loader uses explicit boolean gates; it does not infer enablement from payload presence.
- `movement_enabled=true`: load authored movement only (no synthesized fallback movement payload).
- `hitbox_enabled=false`: skip hitbox parsing and keep runtime hitbox caches empty.
- `attack_box_enabled=false`: skip attack-box parsing and keep runtime attack-box caches empty.
- `impassable_enabled=false`: skip impassable-shape load into runtime state.
- `floor_boxes_enabled=false`: skip floor-box load into runtime state.

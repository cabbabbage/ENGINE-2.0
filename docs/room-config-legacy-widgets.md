# Legacy Room Config Widget Notes (Archived)

This document captures the old Room/Trail config panel at a high level so a future replacement can mirror capability without reusing the broken implementation.

## Sections (High-Level)

- `Room Geometry`
- `Tags / Types`
- `Spawn Groups`
- `Trail Connection Sector` (context-dependent)

## Widget Types and Intent

- `Text input`
  - Controlled display name for the selected room/trail.
- `Dropdown`
  - Controlled geometry mode selection (for example square/circle-style shape mode).
- `Range sliders`
  - Controlled min/max size bounds for width and height.
- `Single-value slider`
  - Legacy surface parameters (removed): replaced by weighted coarseness ranges; edge detail candidates are configured once at map level, not per room/trail.
- `Checkboxes`
  - Controlled boolean room flags (boss-like marker, inherit-assets style flags).
- `Tag editor chips/list`
  - Controlled include/exclude-style tagging metadata for room/trail classification.
- `Spawn-group list rows`
  - Displayed configured spawn groups and allowed select/open/delete style actions.
- `Directional sector visual + steppers`
  - Controlled sector direction and sector width percentages for trail-connection metadata.
- `Reset button`
  - Reset sector controls back to full/default sector values.

## Data Domains Formerly Controlled

- Name / identity metadata.
- Geometry and dimension bounds.
- Edge/surface shaping metadata: rooms/trails keep weighted coarseness ranges, while `edge_detail_candidates` belongs to map-level generation data and runs after coarseness expansion.
- Room/trail tags and classification flags.
- Spawn-group references and list actions.
- Trail-connection directional sector metadata.

## What Not To Reuse

- Old event-routing and focus/capture behavior.
- Old shared/duplicated context branching between room and trail flows.
- Old panel lifecycle coupling across footer/layers/editor entry points.

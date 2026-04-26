# Room Editor UI Consistency Checklist

Use this checklist for every room-editor panel change in `ENGINE/editor/devtools`.

## Layout Contract
- Panel follows section order: `Header/Status -> Item List -> Details -> Actions -> Propagation`.
- Section names are consistent with existing panel vocabulary.
- List, detail, and action regions keep consistent spacing/padding from shared style constants.
- Action buttons keep stable ordering (`Add`, `Rename/Apply`, `Delete`, `Propagate` as applicable).

## Behavior Contract
- Keyboard tab traversal is deterministic and left-to-right, top-to-bottom within section order.
- Enter commits focused editable details/actions.
- Escape cancels active field editing first; second escape closes transient candidate popups.
- Hover/selection visuals match existing panel affordances.

## Ownership and Safety Contract
- Operations only surface entities owned by the active mode/domain.
- Candidate editor context includes source type and rejects mismatches.
- Delete actions show affected count/type/scope before commit.
- Cancel path guarantees zero mutation.
- Confirm path mutates only validated, in-scope targets.

## Regression Contract
- Add/update tests for mode isolation and source/domain validation.
- Add/update tests for cancel/confirm delete safety where delete behavior changed.
- Verify unchanged payload keys remain byte-equal for writer contract tests.

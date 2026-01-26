Child Asset System Refactor Plan

Objective
Refactor the child asset system to enforce a clean separation of responsibilities:

- Parent responsibility: create and spawn children only when the parent becomes active (one-time action)
- Child responsibility: every frame, read parent state for transform and visibility, and manage its own lifecycle
- No parent-side management after spawn: parent does not track, update, sync, or delete children after they are created

Core Rules
- Child assets must be created and finalized when the parent is activated
- Child assets must be added to the active assets list when spawned
- Parent must not “own” children after creating them
- If a parent is activated, then deactivated, then activated again, it must spawn fresh children again
- Deactivating a child asset must delete the child asset
- Child assets must not be created during map generation or map load
- Child assets must not be part of the all-assets list

--------------------------------------------------------------------

Task 1: Spawn children on parent activation
- When a parent asset becomes active, create an instance of all child assets required by its child timelines this is not to be owned by the parent asset at alllll tho it is to be passed directly to the acrive assets  list but th children should be created so that they know who the parent is, but from this point forward the parent has nothing to do with the children at all until it get reactivated again at which poijnt it will need to recreate the  children or something.
- Ensure each spawned child is finalized immediately after being created
- Add each child directly into the active assets list again the parent DOES NOT OWN THE ASSEETSSPECIFIED BY THE CHILD TIMES LINES IT SIMPLY CREATES THEM
- Parent does not keep a managed child list for runtime ownership

--------------------------------------------------------------------

Task 2: Child assets sample parent state every frame
- In Asset::update(), if an asset has a parent, it must read its child timeline slot from the parent every frame
- Compute child visibility based on the parent (hidden/inactive rules apply)
- Compute child world position using the parent world transform plus dx/dy/dz
- Apply world_z before the rest of Asset::update runs
- Remove any other system that sets child transform or visibility from the parent side

--------------------------------------------------------------------

Task 3: Child assets self-deactivate when parent deactivates
- If a child detects its parent is inactive, the child must deactivate itself the parent does not call a deactivation of its children

---------------------------------------------------------------

be agressive as fuck to acchievee this child parent interaction where there is no direc ownership of children the parnet only calls for children to be crrated and shit get rid of this transient child shit. parent only onws thechild timeline objects not the actual asset object to be used as chuildren
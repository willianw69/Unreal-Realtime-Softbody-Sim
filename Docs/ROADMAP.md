# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-23 (SB-M4 complete + verified in-editor — core feature set done).

| Milestone | Title | Status |
|---|---|---|
| M0 | Project bootstrap (C++ host builds) | ✅ |
| SB-M1 | GPU lattice solid (framework port + distance constraints) | ✅ |
| SB-M2 | Volume constraints → jelly (tetrahedra) | ✅ |
| SB-M3 | Mouse dragging (pick + grab constraint) | ✅ |
| SB-M4 | Collisions (ground / sphere / capsule / self) | ✅ |
| SB-M+ | XPBD compliance, Sphere/SDF shapes, mesh embedding, profiling | ⏳ (stretch) |

## Notes per Milestone

### M0 — Project bootstrap ✅
`SoftBodyDemo` UE 5.7 C++ host project created (uproject, Target.cs ×2, Build.cs, module, Config,
.gitignore/.gitattributes) mirroring `ClothSimDemo`. Builds clean via CLI. No plugin yet.

### SB-M1 — GPU lattice solid ✅ (verified in-editor 2026-06-22)
Created `Plugins/SoftBodySim` by porting the ClothSim GPU framework. Runtime-generates a 3D particle
**lattice** (centered box), the **6-tet Kuhn split** of every cube cell, and **distance constraints**
deduped from the unique tet edges (cube edges + face diagonals + body diagonal), greedily graph-colored.
Pipeline: `SBPredict` (gravity+damping) → colored Gauss-Seidel distance solve (in place) → `SBCollision`
(ground plane) → `SBFinalize` → readback. Renders the box boundary surface as a lit mesh via the ported
`FSoftBodyMeshSceneProxy` (`Cross(E2,E1)` smooth normals, winding oriented outward). Built-in ground plane
+ an `Anchor` option (None / Top face / Bottom face). **Result:** a springy deformable box that falls,
jiggles/sags, and rests on the floor (volume not yet preserved — expected). The full 3D GPU pipeline works.
Note: tetrahedra are already built here (not deferred to SB-M2) because the distance constraints derive
from tet edges — so SB-M2 only needs to add a second (volume) constraint set over the existing `Tets`.

### SB-M2 — Volume constraints → jelly ✅ (verified in-editor 2026-06-22)
Added per-tet **volume constraints** (`SBSolveVolume.usf`) over the `Tets` array built in SB-M1,
graph-colored as a **second** constraint set (its own color ranges + per-color dispatches; two tets
conflict if they share any of their 4 vertices). Rest volume `V0 = (1/6)·dot(e1, e2×e3)`; PBD
`C = V − V0`, gradients = cross products of opposing edges, all 4 verts moved mass-weighted, race-free
per color. Each solver iteration relaxes distance colors then volume colors so they converge together.
Exposed a `VolumeStiffness` [0..1] knob. **Result:** the box keeps its volume — squashes and bulges
back like jelly. The A/B test (VolumeStiffness 0 = SB-M1 flatten, 1 = volume-preserving) is the
headline demo. The defining "Obi/Zibra-style" behaviour is now in.

### SB-M3 — Mouse dragging ✅ (verified in-editor 2026-06-23)
Deproject cursor → world ray; on click pick the nearest **boundary** particle in front of the camera
(CPU, from the position readback) and remember its index + grab depth; while held, the target tracks the
cursor at that depth. A one-thread `SBGrab.usf` pass pulls the grabbed particle toward the target
(`lerp(p, target, GrabStiffness)`), run after the constraint solve and before collision so the grab is
the solver's last word yet still respects the ground; neighbours follow via the constraints (stretchy
drag), and Finalize gives momentum on release. Cursor enabled at BeginPlay; yellow sphere/line feedback.
`bEnableMouseDrag` / `GrabStiffness` / `GrabPickRadiusScale` knobs. Reuses the existing readback +
param-push — no extra GPU readback. Interactive poke/pull/stretch works.

### SB-M4 — Collisions ✅ (verified in-editor 2026-06-23)
Sphere/capsule colliders (Details-panel `FSoftBodyCollider` array → world-space `FGPUCollider` buffer →
the existing `SBCollision.usf` capsule routine; yellow wireframe debug draw) plus GPU spatial-hash
**self-collision** (`SBBuildGrid.usf` atomic broadphase + `SBSelfCollision.usf` 27-cell Jacobi repulsion,
ported from cloth with the neighbour exclusion adapted to the lattice 1-ring via ResX/Y/Z). Self-collision
ping-pongs `PredictedA`/`PredictedB` so the gather reads a clean snapshot; runs per substep before
grab/collision. Knobs: `Colliders`, `bDrawColliders`, `bSelfCollision`, `SelfCollisionScale`/`Stiffness`/
`Iterations`. The jelly drapes/squashes over shapes and resists interpenetrating itself under compression.
This completes the Obi/Zibra-style core feature set.

### SB-M+ — Stretch ⏳
XPBD compliance per constraint (proper stiffness independent of iteration count); Sphere/arbitrary
shapes via SDF voxelization + boundary-from-tets extraction; embed a smooth render mesh skinned to
the tets; multiple bodies; zero-copy GPU vertex write; `stat GPU`/Insights profiling pass.

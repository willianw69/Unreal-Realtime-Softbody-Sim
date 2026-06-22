# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-22 (SB-M2 complete + verified in-editor).

| Milestone | Title | Status |
|---|---|---|
| M0 | Project bootstrap (C++ host builds) | ✅ |
| SB-M1 | GPU lattice solid (framework port + distance constraints) | ✅ |
| SB-M2 | Volume constraints → jelly (tetrahedra) | ✅ |
| SB-M3 | Mouse dragging (pick + grab constraint) | ⏳ |
| SB-M4 | Collisions (ground / sphere / self) | ⏳ |
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

### SB-M3 — Mouse dragging ⏳
Deproject cursor → ray; pick nearest boundary particle (CPU from readback); push grab target; grab
step pulls it (soft attachment). Interactive poke/pull/stretch.

### SB-M4 — Collisions ⏳
Port the cloth collision passes: built-in ground plane (already in SB-M1), sphere/capsule colliders,
and spatial-hash **self-collision**, so the jelly squashes on the floor and against shapes.

### SB-M+ — Stretch ⏳
XPBD compliance per constraint (proper stiffness independent of iteration count); Sphere/arbitrary
shapes via SDF voxelization + boundary-from-tets extraction; embed a smooth render mesh skinned to
the tets; multiple bodies; zero-copy GPU vertex write; `stat GPU`/Insights profiling pass.

# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-25 (SB-M10 interactive cutting + bActive toggle; SB-M11 queued).

| Milestone | Title | Status |
|---|---|---|
| M0 | Project bootstrap (C++ host builds) | ✅ |
| SB-M1 | GPU lattice solid (framework port + distance constraints) | ✅ |
| SB-M2 | Volume constraints → jelly (tetrahedra) | ✅ |
| SB-M3 | Mouse dragging (pick + grab constraint) | ✅ |
| SB-M4 | Collisions (ground / sphere / capsule / self) | ✅ |
| SB-M5 | Custom mesh embedding (FFD cage) | ✅ |
| SB-M6 | Weight-painted per-region stiffness | ✅ |
| SB-M7 | XPBD compliance (distance solve) | ✅ |
| SB-M8 | Distance-field collision (Global Distance Field) | ✅ |
| SB-M9 | Multi-body collision (shared spatial hash) | ✅ |
| SB-M10 | Interactive cutting + visual split (lattice) | ✅ |
| SB-M11 | Cuttable custom meshes (render-mesh splitting) | ⏳ (next) |
| SB-M+ | Per-mesh/custom SDF colliders, conforming cage, XPBD volume, render-mesh skinning, profiling | ⏳ (stretch) |

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

### SB-M5 — Custom mesh embedding ✅ (verified in-editor 2026-06-23)
Assign a `UStaticMesh` (`SoftBody|Mesh → Source Mesh`): its LOD0 verts/triangles/UVs are read on the CPU
at BeginPlay, the lattice becomes a box **cage** auto-fit to the mesh bounds (ResX/Y/Z = cage density;
`CagePadding`), and each mesh vertex is bound to its containing cage tet by **barycentric weights**
(`BuildEmbedding`, 3×3×3 cell search). Each frame the mesh verts are reconstructed from the deformed cage
and rendered via the existing proxy (free-form deformation). Everything else (constraints, volume, grab,
collisions) runs on the cage unchanged. No mesh → the default box.

### SB-M6 — Weight-painted per-region stiffness ✅ (verified in-editor 2026-06-23)
Paint grayscale **vertex colors** on the Source Mesh (white = soft, black = firm by default); the R channel
is read alongside positions, sampled onto each cage particle by nearest mesh vertex (`BuildParticleWeights`),
and drives a per-constraint softness so different parts of one body are floppier/firmer. `bVisualizeWeights`
tints the debug points (blue→red) to preview the transfer. Knobs: `bWeightPaintStiffness`, `bInvertWeightPaint`.
(PBD stiffness scaling proved too iteration-coupled to show clearly — fixed properly by SB-M7.)

### SB-M7 — XPBD compliance ✅ (verified in-editor 2026-06-23)
Converted the distance solve to **XPBD** (`SBSolveDistanceXPBD.usf` + per-constraint Lagrange-multiplier
buffer reset each substep): stiffness is now a true **compliance** (`α̃ = compliance/dt²`), independent of
iteration/substep count, so weight-painted softness holds at any solver settings (the SB-M6 contrast no
longer washes out). Per-constraint compliance = `XpbdGlobalCompliance + Softness·XpbdSoftCompliance`.
`bUseXPBD` toggles vs the old PBD path (compliance 0 ≡ rigid, so the default matches SB-M1..M6); volume
stays PBD. The main softness dial is now `XpbdSoftCompliance`.

### SB-M8 — Distance-field collision ✅ (verified in-editor 2026-06-23)
Collide the body against ANY scene mesh via Unreal's **Global Distance Field** — ported from the cloth
sim. `FSoftBodySceneViewExtension` (`SoftBodyGDF::EnsureRegistered`/`Get`) snapshots the GDF parameters +
view each frame in `PostRenderBasePassDeferred_RenderThread`; `SBCollisionDF.usf` samples
`GetDistanceToNearestSurfaceGlobal` + gradient per particle and projects out of the contact shell (+
friction). `FSBCollisionDFCS` binds `FGlobalDistanceFieldParameters2` + the View UB; the pass runs after
analytic collision, on `Solved`. Knobs `bUseDistanceFieldCollision`/`DistanceFieldThickness`; on-screen GDF
diagnostic. Requires "Generate Mesh Distance Fields" (now set in `DefaultEngine.ini`). Additive with the
ground + sphere/capsule colliders. **Also in this milestone:** fixed a culling bug (bounds now recompute
every frame from the deformed body + push to the render thread, so it no longer vanishes when it moves far
from the actor); raised the cage `ResX/Y/Z` cap 32 → 64.

### SB-M9 — Multi-body collision ✅ (verified in-editor 2026-06-24)
A `USoftBodyWorldSubsystem` (UWorldSubsystem + FTickableGameObject) registers every live component and, once
per frame after all bodies sim, enqueues one GPU pass (`SoftBodyCompute::DispatchInterBody_RenderThread`):
concatenate every participating body's committed Positions/InvMasses into combined buffers (`AddCopyBufferPass`)
+ a per-particle body-id, build a **shared spatial hash** over all of them (reusing `SBBuildGrid.usf`), and
run `SBInterBodyCollide.usf` to repel particles of DIFFERENT bodies apart (same-body pairs skipped — handled
by self-collision), ping-ponging over `InterBodyIterations`; then copy the corrected positions back into each
body's Positions buffer. A post-sim positional projection, so each body's substep is untouched. Opt-in per
body via `bInterBodyCollision` (+ `InterBodyThickness`/`InterBodyStiffness`/`InterBodyIterations`; the
subsystem uses the max across participants). Bodies pile/squash together. ~1-frame lag (acceptable for soft
contact); particle-level so resolution tracks cage density.

### SB-M10 — Interactive cutting + visual split ✅ (verified in-editor 2026-06-25)
Right-click-drag a stroke to slice a **box/lattice** body: the swipe defines a plane (through the camera,
spanning the start + end rays), and every distance/volume constraint crossing it is severed so the body
splits into independent chunks. Mechanism: per-constraint **broken** flags (`DistanceBrokenBuffer`/
`VolumeBrokenBuffer`, zeroed at init) that the three solve shaders skip; the cut is tested CPU-side from the
readback positions, the flags re-uploaded (`UpdateBrokenState_RenderThread`), and the surface re-extracted
from the surviving tets' boundary faces (`BuildTetBoundarySurface`, faces used by exactly one un-cut tet) with
a dynamic index buffer (`FSoftBodyMeshSceneProxy::UpdateIndices_RenderThread`) so new cut faces appear. Cuts
accumulate; coarse cages give chunkier cut faces. **Lattice only** — an embedded Source Mesh tears physically
but its surface stretches at the seam (clean mesh cutting is SB-M11). Also added a component **`bActive`**
master switch (evaluated at BeginPlay) to disable specific actors for testing.

### SB-M11 — Cuttable custom meshes ⏳ (next)
Extend SB-M10's cut to **embedded Source Meshes** so the render mesh opens cleanly, not just the cage. The
cage already separates physically (constraints break); what's missing is **splitting the render mesh's
triangles along the cut plane**: for triangles straddling the plane, duplicate vertices along the seam,
retriangulate the two sides, generate the new cut-face geometry, and re-embed the new verts into the cage
tets (barycentric). Edge cases: UV/normal seams, multiple accumulating cuts, verts exactly on the plane.
Meaty — worth a plan pass before implementing.

### SB-M+ — Stretch ⏳
Higher-fidelity colliders than the GDF for a specific mesh (custom baked SDF / per-mesh distance field);
conforming (non-box) cage via SDF voxelization + boundary-from-tets; a higher-quality render-mesh skinning
upgrade (smooth-bind / multi-tet weights); XPBD for the volume solve too; broadphase for many inter-body
bodies; zero-copy GPU vertex write; `stat GPU`/Insights profiling pass.

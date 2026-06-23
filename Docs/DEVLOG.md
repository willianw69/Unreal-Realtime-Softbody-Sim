# DEVLOG.md

> Chronological development history. Append new entries; never replace old ones.
> Format per entry: date — what / why / problems & solutions / performance / next.

---

## 2026-06-22 — M0: Project bootstrap + design
**What:** Created the host project `SoftBodyDemo` (UE 5.7 C++) at `E:\ClaudeCode\RT_SoftBody`:
`SoftBodyDemo.uproject`, `Source/SoftBodyDemo.Target.cs` + `SoftBodyDemoEditor.Target.cs`,
`Source/SoftBodyDemo/SoftBodyDemo.Build.cs` + module `.cpp/.h`, `Config/` (DefaultEngine/Game/
Input/Editor), `.gitignore` + `.gitattributes`. Mirrors the proven `ClothSimDemo` scaffolding
(BuildSettings V6, DX12, `r.ShaderDevelopmentMode=1`); dropped the cloth-specific distance-field
cvars. Established the full design + this `Docs/` set for the tetrahedral XPBD soft body.

**Why:** Stand up a compiling C++ project before building the simulation plugin, and capture the
plan/architecture/workflow so the work can be handed to a fresh session with zero prior context.

**Problems & solutions:** None — built first try via CLI (UHT + module → `UnrealEditor-SoftBodyDemo.dll`).

**Performance:** N/A (empty host module).

**Next:** SB-M1 — create the `SoftBodySim` plugin (port the ClothSim framework), runtime lattice +
distance constraints (colored Gauss-Seidel), predict/finalize/readback, render the box surface.

---

## 2026-06-22 — SB-M1: GPU lattice solid (framework port)
**What:** Created `Plugins/SoftBodySim` by porting the ClothSim GPU framework, and got the first
soft body simulating + rendering. Files: `SoftBodySim.uplugin` (PostConfigInit) + module mapping
`/SoftBodySim` → `Shaders/`; `SoftBodySim.Build.cs` (Core/CoreUObject/Engine + Projects/RenderCore/
RHI/Renderer); `SoftBodyResources.h` (`FSoftBodyParams`, `FGPUConstraint`, `FSoftBodyColorRange`,
`FGPUCollider`, `FSoftBodyTet`, `FSoftBodyRenderResources` + out-of-line ctor/dtor); 4 shaders
`SBPredict.usf` (gravity+damping), `SBSolveDistance.usf` (= cloth colored Gauss-Seidel, verbatim),
`SBCollision.usf` (ground plane + collider plumbing), `SBFinalize.usf` (v=(p−x)/dt); `SoftBodyCompute.cpp`
(shader classes + `IMPLEMENT_GLOBAL_SHADER` + `InitResources`/`Dispatch`, RDG pipeline); `USoftBodyComponent`
(UMeshComponent); `FSoftBodyMeshSceneProxy` (FLocalVertexFactory); `ASoftBodyActor`. Enabled the plugin
in `SoftBodyDemo.uproject`.

The component runtime-generates: a **centered box lattice** (`ResX×ResY×ResZ` @ `Spacing`, base at local
Z=0); the **6-tet Kuhn/Freudenthal split** of every cube cell (all tets share the cell main diagonal →
face-conforming); **distance constraints** deduped from the unique tet edges (a 64-bit (min,max) key over
all 6 edges of each tet → cube edges + 6 face diagonals + 1 body diagonal, rest length from rest geometry);
**greedy graph coloring** (same algorithm as cloth `BuildConstraints`); and the **boundary surface** (the 6
outer faces, two tris per quad, winding chosen per-quad so `Cross(E2,E1)` faces outward). Per substep on the
render thread: Predict → for iterations { for colors: in-place GS distance solve } → ground collision →
Finalize → readback. Vertex i == particle i (interior particles sit in the vertex buffer but aren't indexed),
so the readback → local → smooth-normal → proxy-upload path is trivial. Fixed-timestep accumulator (1/60).

**Why:** Establish the full 3D GPU soft body pipeline end-to-end (lattice → constraints → colored GPU
solve → readback → lit mesh) before adding the headline volume constraints. Deliberately reuses ~75% of
the cloth framework (RDG dispatch, pooled buffers, colored GS solver, readback rendering, proxy).

**Problems & solutions:**
- *Build error C2672/incomplete-type on `~TArray<FSoftBodyTet>`*: the component stores `TArray<FSoftBodyTet> Tets`
  as a member, but `FSoftBodyTet` was only forward-declared in the header, so the UHT-generated destructor
  couldn't destruct the array. Fix: `#include "SoftBodyResources.h"` in `SoftBodyComponent.h` (cloth dodged
  this because its forward-declared structs were only used in function signatures, never as value members).
- Built tetrahedra in SB-M1 (not deferred to SB-M2) since the distance constraints derive from tet edges;
  this also pre-stages SB-M2 (volume just adds a second constraint set over the existing `Tets`).
- Applied the inherited gotchas from day one: `Cross(E2,E1)` smooth normals + two-sided material;
  `RDG_EVENT_NAME` only (no `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` on the standalone builder); out-of-line
  resources ctor/dtor for the `TUniquePtr<FRHIGPUBufferReadback>`; shader paths include the `/Private/` subfolder.

**Performance:** Not yet profiled. Default demo = 5×5×5 (125 particles, 384 tets, ~ a few hundred unique
edges). The GS solve issues `SolverIterations × NumColors` dispatches per substep (see the on-screen stats).

**Next:** SB-M2 — per-tet volume constraints (`SBSolveVolume.usf`) as a second colored constraint set over
the existing `Tets`, for true volume-preserving jelly.

---

## 2026-06-22 — SB-M2: volume constraints → jelly
**What:** Added the headline feature — per-tetrahedron **volume constraints** as a second
graph-colored constraint set, on top of SB-M1's distance edges. New shader `SBSolveVolume.usf`
(+ `FSBSolveVolumeCS`); new `FGPUVolumeConstraint` struct (24-byte tight layout: 4 uint indices +
rest volume + stiff scale) and a second pooled `VolumeConstraintsBuffer` / `NumVolumeConstraints` /
`VolumeColorRanges` on `FSoftBodyRenderResources`; a `VolumeStiffness` [0..1] param + `UPROPERTY`.
`USoftBodyComponent::BuildVolumeConstraints` computes each tet's signed rest volume
`V0 = (1/6)·dot(e1, e2×e3)` from the rest lattice and **greedily colors the tets** (two tets conflict
if they share ANY of their 4 vertices — the 4-wide analogue of the edge coloring). The dispatch now
interleaves, per iteration: all distance colors, then all volume colors, solving both in place on the
single `Predicted` buffer.

The solve itself (PBD volume constraint, Müller et al.): `C = V − V0`; per-vertex gradients
`g1=(1/6)e2×e3, g2=(1/6)e3×e1, g3=(1/6)e1×e2, g0=−(g1+g2+g3)`; scale `s = −C / Σ w_k|g_k|²`;
correction `dp_k = K·s·w_k·g_k` with `K = saturate(Stiffness·VolumeStiffness·StiffScale)`. Pinned
verts (w=0) contribute nothing; color guarantees the 4 writes per thread are disjoint → race-free.

**Why:** Volume preservation is the defining "solid jelly" behaviour (Obi/Zibra-style) and the
whole reason for choosing a tetrahedral model over cloth-style distance-only or pressure/shape-matching.
SB-M1 deliberately pre-built the `Tets`, so this milestone was purely "add a second constraint set" —
no new topology, validating that the colored-Gauss-Seidel framework generalizes across constraint types.

**Problems & solutions:** None — compiled + linked first try. The structured-buffer stride gotcha was
pre-empted by using a tight scalar struct (24 B) that matches between C++ and HLSL (structured buffers
pack tightly; the 16-byte-alignment trap is constant-buffer-only). A/B verified in-editor: VolumeStiffness
0 reproduces SB-M1 flattening, 1 holds volume and bulges back.

**Performance:** Not yet profiled. Default 5×5×5 = 384 tets in a handful of colors; solve dispatches per
substep = `iters × (distanceColors + volumeColors)` (shown in the on-screen stats). Volume roughly
doubles the per-iteration dispatch count vs SB-M1.

**Next:** SB-M3 — mouse dragging: deproject cursor → ray, pick nearest boundary particle from the
readback, push a grab target to the render thread, pull that particle (soft attachment / temp pin).

---

## 2026-06-23 — SB-M3: mouse dragging
**What:** Made the soft body interactive — left-click and drag to poke/pull/stretch it. New shader
`SBGrab.usf` (+ `FSBGrabCS`): a single GPU thread pulls the grabbed particle toward a world-space cursor
target, `PredictedPositions[GrabIndex] = lerp(P, GrabTarget, saturate(GrabStiffness))`. Added grab fields
to `FSoftBodyParams` (`bGrabActive`, `GrabIndex`, `GrabTarget`, `GrabStiffness`). The dispatch runs the
grab pass AFTER the distance+volume solve and BEFORE collision, so the grabbed vertex is the solver's last
word yet is still projected out of the ground; neighbours follow via the constraints over substeps, and
Finalize (`v=(p−x)/dt`) gives momentum on release.

CPU side (`USoftBodyComponent::UpdateMouseGrab`, called from Tick): build a `BoundaryParticles` list (any
lattice particle on a box face) once at init; on left-mouse-down deproject the cursor
(`DeprojectMousePositionToWorld`) to a world ray and pick the boundary particle with the smallest
point-to-ray distance that's in front of the camera (within `GrabPickRadiusScale * Spacing`), remembering
its index + grab depth (`dot(P−rayO, rayDir)`); while held, the target = `rayO + rayDir * depth` (follows
the cursor in the view plane at the picked depth); on release, clear the grab. Cursor enabled at BeginPlay.
Yellow debug sphere at the target + line to the grabbed particle. Props: `bEnableMouseDrag`,
`GrabStiffness`, `GrabPickRadiusScale`.

**Why:** Interactive dragging is a stated project requirement and the best way to *show* the volume
preservation — you can stretch the jelly and watch it recoil. Reuses the existing position readback (for
picking) and the per-frame param-push (for the target), so no new GPU→CPU traffic was needed.

**Problems & solutions:** Link error `LNK2019: EKeys::LeftMouseButton` — `EKeys` lives in the **InputCore**
module, which the plugin didn't depend on (the host module did, but not the plugin). Fix: add `InputCore`
to `PublicDependencyModuleNames` in `SoftBodySim.Build.cs`. Otherwise compiled + ran cleanly.

**Performance:** Picking is CPU brute-force over the boundary particles (the surface subset, not all
particles) — trivial at demo resolutions; would want a spatial structure only at very high res. The grab
pass is one thread.

**Next:** SB-M4 — collisions: sphere/capsule colliders (Details-panel authored → `FGPUCollider` buffer;
`SBCollision.usf` already has the capsule routine) + GPU spatial-hash self-collision (port cloth
`ClothBuildGrid`/`ClothSelfCollision`).

---

## 2026-06-23 — SB-M4: collisions (sphere/capsule + self)
**What:** Added shape colliders and self-collision, completing the core feature set.
*Sphere/capsule colliders:* new `FSoftBodyCollider` USTRUCT (+ `ESoftBodyColliderType`) and a
`Colliders` array UPROPERTY; in `TickComponent` each is transformed to a world-space `FGPUCollider`
(sphere → A==B; capsule → A/B = center ± axis·halfHeight) and appended to `Params.Colliders`. The
existing `SBCollision.usf` (ported in SB-M1 with the full capsule routine) consumes them with no shader
change — only wiring + a `DrawColliders` wireframe helper. *Self-collision:* ported `ClothBuildGrid.usf`
→ `SBBuildGrid.usf` (atomic spatial-hash broadphase) and `ClothSelfCollision.usf` → `SBSelfCollision.usf`
(27-cell Jacobi repulsion), plus `FSBBuildGridCS`/`FSBSelfCollisionCS` and the `NextPrime`/`kMaxPerCell`
helpers. Adapted the neighbour-exclusion from the cloth's 2D grid 1-ring to the soft body's **3D lattice
1-ring** (decompose flat index → x/y/z via `ResX/Y/Z`, skip if Chebyshev distance ≤ 1). Added lattice dims
+ `bSelfCollision`/`SelfThickness`/`SelfStiffness`/`SelfCollisionIterations` to `FSoftBodyParams`, and the
matching component props (`SelfCollisionScale` → thickness = scale·Spacing).

*Dispatch restructure:* the single in-place `Predicted` buffer became `PredictedA`/`PredictedB`. A
`Solved` ref tracks the current buffer through the substep: the colored solves run in place on PredictedA,
then (if enabled) self-collision ping-pongs `Solved → Other` per iteration (build grid from Solved →
repulse into Other → `Solved = Other`), then grab → collision → finalize all read/write `Solved`.
Self-collision runs before grab/ground so a hard pin or solid floor still gets the final say.

**Why:** Collisions make the jelly interact with the world (drape/squash over shapes) and with itself
(no interpenetration under compression) — the last piece of the Obi/Zibra-style target. Maximal reuse:
the analytic-collider shader was already in place from SB-M1, and the self-collision passes are the
cloth's spatial hash with a one-line topology tweak — another data point that the framework generalizes.

**Problems & solutions:** None — compiled + linked first try. The buffer-aliasing question (self-collision
needs a clean read snapshot) was handled by reintroducing the second predicted buffer and threading a
`Solved` ref, mirroring the cloth M9 ping-pong.

**Performance:** Not yet profiled. Self-collision adds a build + scan pass per substep × `SelfCollisionIterations`;
`MAX_PER_CELL = 16`, table size = `NextPrime(2N)`. Fine at demo resolutions; the broadphase drops pairs on
bucket overflow in very dense piles (documented).

**Next:** Core milestones (SB-M1–M4) complete — the four user requirements + collisions are delivered.
Remaining is the SB-M+ stretch list: XPBD compliance, SDF/non-box shapes, render-mesh embedding/skinning,
multiple bodies, zero-copy vertex write, profiling pass.

---

## 2026-06-23 — SB-M5: custom mesh embedding (FFD cage)
**What:** You can now assign a `UStaticMesh` (`SoftBody|Mesh → Source Mesh`) and simulate it. The lattice
becomes a box **cage** auto-fit to the mesh's bounding box (`CagePadding` expands it); ResX/Y/Z control cage
density. `ReadSourceMesh` pulls LOD0 positions/indices/UV0 (and vertex colors, used by SB-M6) off the CPU;
`BuildEmbedding` binds each mesh vertex to the cage tetrahedron containing it via barycentric weights
(searching the 3×3×3 cell block around the vertex's cage cell; clamps to the nearest tet if slightly outside).
Each frame `UpdateMeshFromSimulation` reconstructs every mesh vert from its tet's 4 deformed cage particles
and recomputes normals from the mesh's own triangles. The mesh proxy is already generic, so it renders the
embedded mesh directly. `ComputeSurfaceNormalsTangents` was generalized to `ComputeNormalsTangents(positions,
tris, …)`. `EffectiveSpacing` (min cage cell size) now drives self-collision thickness + grab pick radius.

**Why:** "Use a custom mesh as the soft body" — the standard, robust approach is cage-based free-form
deformation (what Obi and lattice deformers do): simulate a coarse tet cage, ride the high-res mesh along
by barycentric weights. Chosen over conforming/SDF cages (a stretch item) for simplicity and concave-mesh
robustness, and it reuses the entire existing sim untouched — only the render-vertex sourcing changed.

**Problems & solutions:** None at build. Design choice: cage stays a box, so a coarse cage deforms blobbily
(documented; raise Res to follow the silhouette). Vertices outside all cage tets (padding/concavity) clamp
their barycentric weights to the nearest tet so they stay glued. CPU mesh read needs render data available
(fine in editor; packaged builds want Allow CPUAccess).

**Performance:** Embedding is built once at init (cell-local search, ~bounded per vertex). Per frame:
O(mesh verts) reconstruct + normals on the game thread. Heavy for very high-poly meshes; fine for demo assets.

**Next:** SB-M6 — paint per-region softness via the mesh's vertex colors.

## 2026-06-23 — SB-M6: weight-painted per-region stiffness
**What:** Painted grayscale **vertex colors** on the Source Mesh now make different regions floppier/firmer.
`ReadSourceMesh` also reads the LOD0 ColorVertexBuffer (R channel → per-vert weight 0..1).
`BuildParticleWeights` samples that onto each cage particle by nearest mesh vertex. `BuildConstraints` /
`BuildVolumeConstraints` derive a per-constraint softness from the endpoint weights. `bVisualizeWeights`
tints the debug points blue(firm)→red(soft) to preview the transfer. Knobs: `bWeightPaintStiffness`,
`bInvertWeightPaint` (default white = soft).

**Why:** Art-directable softness ("only the ears jiggle") is a core soft-body authoring feature and the
natural use of the cage + a paint channel. Vertex colors are the most paint-like source for a static mesh
(paint in the Static Mesh editor or DCC), and the cage already gives us a place to store the sampled weight.

**Problems & solutions:** First pass mapped weight → **PBD stiffness scale**, but the contrast was barely
visible: PBD stiffness is applied per-iteration, so over 8 iters × 2 substeps even a 0.1 scale converges
~80% and "soft" looks like "firm". The data path was correct (the weight visualizer confirmed it) — the
*lever* was wrong. Fixed properly in SB-M7 by switching to XPBD compliance.

**Performance:** Weight transfer is O(cage particles × mesh verts) at init (brute-force nearest) — fine at
demo sizes; would want a grid accel at very high res/poly.

**Next:** SB-M7 — XPBD so the painted softness is iteration-count-independent.

## 2026-06-23 — SB-M7: XPBD compliance (distance solve)
**What:** Rewrote the distance solve as **XPBD** (`SBSolveDistanceXPBD.usf` + `FSBSolveDistanceXPBDCS`),
selected by a `bUseXPBD` toggle (default on; the original PBD shader stays for comparison). Each constraint
now has a Lagrange multiplier `λ` (a typed-float RDG buffer, sized to the constraint count, cleared to 0 at
the start of every substep via a uint UAV view) accumulated across the substep's iterations:
`α̃ = compliance/dt²`, `Δλ = (−C − α̃·λ)/(wa+wb+α̃)`, `λ += Δλ`, `pa += Δλ·wa·n`, `pb −= Δλ·wb·n`.
Per-constraint compliance = `XpbdGlobalCompliance + Softness·XpbdSoftCompliance`, where `Softness` is the
painted weight baked into `FGPUConstraint` (struct grew 16→20 B; both distance shaders' `FConstraint` updated
to match). Volume solve stays PBD.

**Why:** XPBD makes stiffness a true **compliance** (inverse material stiffness) that is independent of
iteration and substep count — `α̃ = α/dt²` exactly cancels the iteration coupling that washed out SB-M6.
With it, weight paint produces a strong, predictable firm↔soft difference at any solver settings; compliance
0 reproduces a fully stiff PBD projection, so the default (unpainted, global compliance 0) matches SB-M1..M6
behaviour and nothing regresses.

**Problems & solutions:** None at build. λ reset handled by clearing the typed float buffer through a
`PF_R32_UINT` UAV view to `0u` (0.0f bit pattern), avoiding an engine-version-specific float-clear overload;
the solve binds a `PF_R32_FLOAT` UAV of the same buffer. Graph coloring already guarantees each constraint
(hence each λ slot) is touched by exactly one thread per color dispatch, so λ reads/writes are race-free.

**Performance:** Same dispatch count as the PBD distance path + one tiny per-substep clear; an extra λ
read/write per constraint. Negligible.

**Next:** Core + mesh + art-direction milestones complete. Remaining is the SB-M+ stretch list (SDF cage
shapes, XPBD volume, render-mesh skinning upgrade, multiple bodies, zero-copy verts, profiling).

---

<!-- Append SB-M+ / further entries below after each is implemented + verified in-editor. -->

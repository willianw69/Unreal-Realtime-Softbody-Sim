# PROJECT_STATE.md

> Single source of truth for current project status. Update after every milestone.
> Last updated: 2026-06-23 (SB-M5/M6/M7 complete + verified — custom mesh, weight paint, XPBD).

## Project Overview
Real-time **GPU soft body simulation built from scratch** in **Unreal Engine 5.7**, as a
Technical Artist portfolio piece and a sibling to the completed `RT_ClothSim` project. The soft
body is simulated entirely with **custom compute shaders** (no Chaos): particle positions/
velocities live in GPU structured buffers, constraints are **generated at runtime** and solved on
the GPU with **tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints for jelly-like
volume preservation), and the result is rendered as a dynamic lit mesh. Interactive **mouse
dragging** lets you poke and pull the body. Target feel: Unity **Obi Softbody** / **Zibra Soft Body**.

- **Host project:** `SoftBodyDemo` (UE 5.7 C++) at `E:\ClaudeCode\RT_SoftBody`.
- **Plugin (all the real work):** `Plugins/SoftBodySim` (to be created in SB-M1).
- **Engine install:** `E:\Epic Games\UE_5.7`.
- **Sibling project to reuse from:** `E:\ClaudeCode\RT_ClothSim` (plugin `ClothSim`, milestones
  M1–M9 complete: GPU XPBD, colored Gauss-Seidel, collisions incl. self-collision, rendering).

## Requirements (user-stated)
Fully GPU simulated · compute-shader based · **no Chaos dependency** · **runtime-generated
constraints** · **mouse dragging** · **volume preservation (jelly)** · UE 5.7 RHI integration ·
similar to Obi/Zibra soft body.

## Current Milestone
**SB-M5/M6/M7 — Custom mesh, weight paint, XPBD: COMPLETE + verified in-editor (2026-06-23).** You can
now assign any Static Mesh and simulate it via a free-form-deformation cage (SB-M5), paint grayscale
vertex colors to make regions floppier/firmer (SB-M6), and the distance solve is XPBD so that painted
softness is iteration-count-independent and holds at any solver settings (SB-M7). With M1–M7, the core
requirements + collisions + custom-mesh + art-directable softness are all delivered.

## Completed Milestones
- **M0 — Bootstrap.** UE 5.7 C++ host project `SoftBodyDemo` (BuildSettings V6, DX12,
  `r.ShaderDevelopmentMode=1`). Mirrors the cloth host scaffolding. Verified compiling.
- **SB-M1 — GPU lattice solid.** Created `Plugins/SoftBodySim` (ported ClothSim framework):
  `SoftBodySim.uplugin`/module (shader path `/SoftBodySim`), `SoftBodyResources.h`, 4 compute shaders
  (`SBPredict`/`SBSolveDistance`/`SBCollision`/`SBFinalize`), `SoftBodyCompute.cpp` (RDG dispatch),
  `USoftBodyComponent`, `FSoftBodyMeshSceneProxy`, `ASoftBodyActor`. Runtime-builds a centered box
  lattice, the 6-tet Kuhn split per cell, deduped distance constraints from tet edges + greedy graph
  coloring, and the boundary surface; pipeline Predict → colored Gauss-Seidel → ground collision →
  Finalize → readback → lit mesh. Built-in ground plane + face anchor option. Verified in-editor.
- **SB-M2 — Volume constraints → jelly.** Added `SBSolveVolume.usf` + `FSBSolveVolumeCS` and a second
  pooled `VolumeConstraintsBuffer`. The component builds one volume constraint per tet (signed rest
  volume) and greedily colors the tets (conflict = shared vertex). The substep solve interleaves
  distance colors then volume colors each iteration. `VolumeStiffness` param + `FGPUVolumeConstraint`
  struct. Volume-preserving jelly verified in-editor.
- **SB-M3 — Mouse dragging.** Added `SBGrab.usf` + `FSBGrabCS` (one-thread pull of the grabbed particle)
  and grab fields on `FSoftBodyParams`. `USoftBodyComponent::UpdateMouseGrab` deprojects the cursor,
  picks the nearest boundary particle (built `BoundaryParticles` list) from the readback, tracks the
  target depth, and pushes it to the grab pass (after solve, before collision). Cursor enabled at
  BeginPlay; `bEnableMouseDrag`/`GrabStiffness`/`GrabPickRadiusScale` props; added `InputCore` dep
  (EKeys). Interactive drag verified in-editor.
- **SB-M4 — Collisions.** Sphere/capsule colliders: `FSoftBodyCollider` struct + `Colliders` array,
  per-frame world-space build into `Params.Colliders` (the existing `SBCollision.usf` handles them),
  `DrawColliders` wireframes. Self-collision: ported `SBBuildGrid.usf`/`SBSelfCollision.usf` (+ `FSBBuildGridCS`/
  `FSBSelfCollisionCS`, `NextPrime`/`kMaxPerCell`) with lattice-1-ring exclusion; per-substep build+respond
  ping-ponging `PredictedA`/`PredictedB`. Params: `bSelfCollision`/`SelfThickness`/`SelfStiffness`/
  `SelfCollisionIterations` + lattice dims `ResX/Y/Z`. Verified in-editor.
- **SB-M5 — Custom mesh embedding.** `SourceMesh` (UStaticMesh) read on CPU (LOD0 pos/tris/UV/colors);
  cage auto-fits to mesh bounds (`CagePadding`); `BuildEmbedding` binds each mesh vert to a cage tet via
  barycentric weights (cell-local search); per-frame reconstruct mesh verts from the deformed cage +
  normals from mesh topology. `FSoftBodyMeshSceneProxy` is generic so it renders the embedded mesh directly.
- **SB-M6 — Weight-painted stiffness.** Read mesh vertex-color R as a per-vert weight; `BuildParticleWeights`
  samples it onto cage particles (nearest vert); per-constraint softness derived from endpoint weights.
  `bVisualizeWeights` debug tint. Knobs `bWeightPaintStiffness`/`bInvertWeightPaint`.
- **SB-M7 — XPBD compliance.** `SBSolveDistanceXPBD.usf` + `FSBSolveDistanceXPBDCS` + a per-constraint
  Lagrange-multiplier buffer (typed float, cleared per substep). Per-constraint compliance from the painted
  Softness; `bUseXPBD` toggle (compliance 0 ≡ rigid PBD). Volume solve stays PBD.

## Next Milestone
**SB-M+ (stretch).** Core + mesh + art-direction milestones are complete. Optional depth: SDF-voxelized
non-box cage shapes (boundary-from-tets); higher-quality render-mesh skinning; XPBD for the volume solve;
multiple bodies; zero-copy GPU vertex write; a `stat GPU`/Unreal Insights profiling pass. See `ROADMAP.md`.
No milestone is currently in progress.

## Technical Decisions
- **Method:** tetrahedral XPBD — a particle lattice filled with tetrahedra; solve **distance**
  (edges) + **volume** (per tet) constraints. True solid jelly with real volume preservation
  (Obi/Zibra-style). Chosen over pressure/balloon (hollow) and shape-matching (indirect volume).
- **Solver:** graph-colored **Gauss-Seidel** ported from cloth M7 — each constraint type (distance,
  volume) is a constraint set with its own greedy coloring + per-color GPU dispatch (race-free). The
  **distance** solve is **XPBD** (SB-M7, per-constraint compliance + λ, iteration-independent stiffness);
  the **volume** solve is still PBD. A `bUseXPBD` toggle keeps the original PBD distance path for comparison.
- **Constraints generated at runtime** from the lattice/tet topology (satisfies the requirement).
- **Tet decomposition:** 6 conforming tetrahedra per cube cell (Kuhn split) so shared faces match.
- **Rendering:** reuse the cloth's reliable readback → `FLocalVertexFactory` path; render either the lattice
  **boundary surface** (default box) or an **embedded custom mesh** (SB-M5, mesh verts barycentric-skinned to
  the cage tets). Reuse the cloth `Cross(E2,E1)` normal convention so two-sided materials light.
- **Custom mesh:** box-cage **free-form deformation** (auto-fit cage, barycentric embedding). Chosen over
  conforming/SDF cages (a stretch item) for robustness and simplicity — handles concave meshes.
- **Art-directable softness:** painted mesh **vertex colors** → per-cage-particle weight → per-constraint
  XPBD compliance, so softness is paintable and holds independent of solver iteration count.
- **Spaces / timestep:** sim in world space; fixed-timestep accumulator (1/60 s) like cloth.
- **Reuse, don't share:** separate project, so the ClothSim files are **copied + adapted** into a new
  `SoftBodySim` plugin (~70–80% transfers). See `ARCHITECTURE.md` for the reuse map.

## Known Limitations / Notes
- The **distance** solve is XPBD (iteration-independent); the **volume** solve is still PBD, so very high
  `VolumeStiffness` + low Substeps can jitter — raise Substeps (XPBD volume is a stretch goal).
- Collisions are point-based (particles vs analytic shapes / hash grid), not continuous — no CCD, so a
  thin/fast collider can be tunneled; raise Substeps to mitigate. Self-collision drops candidate pairs
  when a hash bucket overflows `MAX_PER_CELL` (documented broadphase approximation).
- Mouse drag picks/moves in the camera view plane at the grab depth (no toward/away-camera pull); CPU
  brute-force nearest-particle pick over the boundary list (fine at demo resolutions).
- Custom-mesh deformation is **cage-based FFD**: a coarse cage looks blobby (raise ResX/Y/Z to follow the
  silhouette); embedding clamps verts that fall outside the cage. The cage is always a box (SDF/conforming
  cages are a stretch item). Mesh CPU read needs render data available (editor OK; packaged needs **Allow
  CPUAccess** on the asset). Weight transfer + per-frame embedding are O(verts) on the CPU.

## Future Improvements
- XPBD for the volume solve too; Sphere/arbitrary cage shapes via SDF voxelization + boundary-from-tets
  extraction; higher-quality render-mesh skinning (smooth/multi-tet bind); multiple bodies; zero-copy GPU
  vertex write; profiling pass.

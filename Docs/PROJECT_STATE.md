# PROJECT_STATE.md

> Single source of truth for current project status. Update after every milestone.
> Last updated: 2026-06-22 (SB-M2 complete + verified in-editor; SB-M3 is next).

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
**SB-M2 — Volume constraints → jelly: COMPLETE + verified in-editor (2026-06-22).** Per-tet volume
constraints now run as a second graph-colored constraint set, so the box preserves its volume —
squashes and bulges back like jelly (the defining Obi/Zibra-style behaviour). A `VolumeStiffness`
knob drives the effect (0 = SB-M1 flatten, 1 = volume-preserving). The headline feature is in.

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

## Next Milestone
**SB-M3 — Mouse dragging.** Deproject the cursor to a world ray; pick the nearest **boundary** particle
(CPU, from the position readback) and remember its index + grab depth; each frame push
`{grabbedIndex, targetWorldPos}` to the render thread and pull that particle toward the target (soft
attachment / temporary pin). Reuses the existing readback + param-push pattern. See `ROADMAP.md`/`HANDOFF.md`.

## Technical Decisions
- **Method:** tetrahedral XPBD — a particle lattice filled with tetrahedra; solve **distance**
  (edges) + **volume** (per tet) constraints. True solid jelly with real volume preservation
  (Obi/Zibra-style). Chosen over pressure/balloon (hollow) and shape-matching (indirect volume).
- **Solver:** graph-colored **Gauss-Seidel** ported from cloth M7 — each constraint type (distance,
  volume) is a constraint set with its own greedy coloring + per-color GPU dispatch (race-free).
  Start with PBD stiffness (reuse cloth math); XPBD compliance (per-constraint λ) is a later upgrade.
- **Constraints generated at runtime** from the lattice/tet topology (satisfies the requirement).
- **Tet decomposition:** 6 conforming tetrahedra per cube cell (Kuhn split) so shared faces match.
- **Rendering:** reuse the cloth's reliable readback → `FLocalVertexFactory` path; render the lattice
  **boundary surface**. Reuse the cloth `Cross(E2,E1)` normal convention so two-sided materials light.
- **Spaces / timestep:** sim in world space; fixed-timestep accumulator (1/60 s) like cloth.
- **Reuse, don't share:** separate project, so the ClothSim files are **copied + adapted** into a new
  `SoftBodySim` plugin (~70–80% transfers). See `ARCHITECTURE.md` for the reuse map.

## Known Limitations / Notes (planned)
- Volume preservation is PBD (iteration-dependent stiffness), not XPBD compliance — very high
  `VolumeStiffness` + low Substeps can jitter; the fix is more Substeps (or XPBD λ, a stretch goal).
- No mouse interaction yet (SB-M3); collisions are ground-plane only so far (sphere/capsule + self in SB-M4).
- Point-based collisions (SB-M4) reuse cloth and are not continuous (no CCD).
- Surface rendering is the lattice boundary; embedding a smooth render mesh skinned to tets is a
  later upgrade.

## Future Improvements
- XPBD compliance per constraint; Sphere/arbitrary shapes via SDF voxelization + boundary extraction;
  render-mesh embedding/skinning; multiple bodies; zero-copy GPU vertex write; profiling pass.

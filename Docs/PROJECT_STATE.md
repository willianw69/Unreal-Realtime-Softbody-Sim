# PROJECT_STATE.md

> Single source of truth for current project status. Update after every milestone.
> Last updated: 2026-06-22 (project bootstrap + plan; SB-M1 not yet started).

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
**M0 — Project bootstrap: COMPLETE.** `SoftBodyDemo` C++ host project created and **builds clean**
via the CLI (UHT + module → `UnrealEditor-SoftBodyDemo.dll`). No plugin yet. Design + docs done.

## Completed Milestones
- **M0 — Bootstrap.** UE 5.7 C++ host project `SoftBodyDemo` (BuildSettings V6, DX12,
  `r.ShaderDevelopmentMode=1`). Mirrors the cloth host scaffolding. Verified compiling.

## Next Milestone
**SB-M1 — GPU lattice solid (framework port).** Create the `SoftBodySim` plugin by porting the
ClothSim GPU framework; generate a 3D particle **lattice** (box) + **distance constraints** at
runtime, graph-colored; run Predict → Solve (colored Gauss-Seidel) → Finalize → readback; render
the box surface as a lit mesh. Result: a springy deformable box (volume not yet preserved — that's
SB-M2). See `ROADMAP.md` and `HANDOFF.md`.

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
- Until SB-M2 (volume constraints) the body holds shape only via distance constraints → it can
  collapse/lose volume.
- Point-based collisions (SB-M4) reuse cloth and are not continuous (no CCD).
- Surface rendering is the lattice boundary; embedding a smooth render mesh skinned to tets is a
  later upgrade.

## Future Improvements
- XPBD compliance per constraint; Sphere/arbitrary shapes via SDF voxelization + boundary extraction;
  render-mesh embedding/skinning; multiple bodies; zero-copy GPU vertex write; profiling pass.

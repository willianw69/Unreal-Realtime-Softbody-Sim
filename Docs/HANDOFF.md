# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-23 (after SB-M4, verified in-editor; core feature set complete — SB-M+ stretch next).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 + SB-M1–M4 COMPLETE & verified in-editor. Core feature set done.** The `SoftBodySim` plugin runs a
  full volume-preserving GPU soft body that is interactively draggable and collides with shapes + itself:
  a tetrahedral lattice box falls, squashes, bulges back to its rest volume (jelly), rests on the ground,
  drapes/squashes over authored sphere/capsule colliders, resists self-interpenetration under compression,
  and can be grabbed and pulled with the mouse. Rendered as a lit two-sided mesh. Builds clean via the CLI
  command below. Pushed to `main`.
- **What exists** (all in `Plugins/SoftBodySim`): plugin/module (`/SoftBodySim`→`Shaders/`),
  `SoftBodyResources.h` (params + `FGPUConstraint` + `FGPUVolumeConstraint` + `FGPUCollider` + `FSoftBodyTet`
  + resources), **8 shaders** (`SBPredict`/`SBSolveDistance`/`SBSolveVolume`/`SBGrab`/`SBBuildGrid`/
  `SBSelfCollision`/`SBCollision`/`SBFinalize`), `SoftBodyCompute.cpp` (RDG pipeline, `Solved`-ref ping-pong
  between `PredictedA`/`PredictedB`), `USoftBodyComponent` (centered box lattice, 6-tet Kuhn split → `Tets`,
  distance constraints from tet edges + greedy coloring, per-tet volume constraints + tet coloring, boundary
  surface, `BoundaryParticles` + `UpdateMouseGrab`, authored `Colliders` + `DrawColliders`, self-collision
  params, fixed-step dispatch, readback→verts), `FSoftBodyMeshSceneProxy`, `ASoftBodyActor`. Demo:
  `Content/M_Softbody` (two-sided material) + a SoftBody actor in `LV_Demo`.
- **Per-substep pipeline:** `SBPredict → [distance colors; volume colors]×iters → [self-collision build+respond
  ×iters, ping-pong] → [SBGrab] → SBCollision (shapes+ground) → SBFinalize → readback`.
- **Key knobs:** `VolumeStiffness` (0 = flatten, 1 = jelly), `Stiffness`, `Substeps`, `SolverIterations`,
  `Anchor` (None/Top/Bottom), `bGroundPlane`/`GroundHeight`, `Friction`, `Colliders[]`/`bDrawColliders`,
  `bSelfCollision`/`SelfCollisionScale`/`SelfCollisionStiffness`/`SelfCollisionIterations`,
  `bEnableMouseDrag`/`GrabStiffness`/`GrabPickRadiusScale`, `bShowStats`/`bDrawDebugPoints`.

## Immediate Next Task — none mandatory; SB-M+ stretch list
All four user requirements (fully GPU, no Chaos, volume-preserving jelly, mouse drag) + collisions are
delivered and verified. No milestone is in progress. Optional next steps, in rough value order — confirm
with the user which (if any) to pursue, then follow `WORKFLOW.md` (build with editor closed → verify → docs/commit):
1. **Profiling pass** — `stat GPU` / Unreal Insights at a few resolutions; record per-pass cost in DEVLOG.
   Cheap, high-signal for the portfolio. Good first pick.
2. **XPBD compliance** — per-constraint λ (distance + volume) so stiffness is iteration-count-independent;
   removes the high-stiffness/low-substep jitter. Add a compliance term to the two solve shaders + an
   accumulated-λ buffer per constraint set; α̃ = compliance/dt².
3. **Non-box shapes** — SDF-voxelize an arbitrary mesh to mask which lattice cells exist, then
   boundary-from-tets surface extraction (faces used by exactly one tet) instead of the 6 box faces.
4. **Render-mesh embedding** — skin a smooth high-res mesh to the tets (barycentric) for nicer visuals
   than the lattice boundary.
5. **Multiple bodies / zero-copy GPU vertex write** (compute writes the vertex buffer directly, dropping
   the readback for rendering — keep a small readback only for mouse picking).
- The cloth project (`RT_ClothSim`) has reference implementations for several of these (XPBD, profiling habits).

## Notes for whoever continues
- Build/run/verify loop + gotchas: see `WORKFLOW.md`. Per-milestone doc+commit gate still applies.
- The simulation is fully working; treat SB-M+ items as independent enhancements, not blockers.

## Inherited gotchas confirmed so far
- A `TArray<T>` **value member** in a `UCLASS` header needs the COMPLETE type `T` (not a forward declaration)
  or the UHT-generated `.gen.cpp` destructor fails with incomplete-type C2672. The component stores
  `TArray<FSoftBodyTet>`, so `SoftBodyComponent.h` includes `SoftBodyResources.h` (where the volume structs live too).
- GPU constraint structs use a **tight scalar layout** that must match between C++ and HLSL (e.g.
  `FGPUVolumeConstraint` = 4×uint + 2×float = 24 B). Structured buffers pack tightly; the 16-byte-alignment
  trap is constant-buffer-only. `Cross(E2,E1)` normals + two-sided material; `RDG_EVENT_NAME` only (no breadcrumb scopes).
- The **plugin links independently of the host module**: using `EKeys` (mouse input) required adding
  `InputCore` to the *plugin's* `Build.cs` even though the host game module already depended on it (SB-M3).

## How to Build & Run
**Close the SoftBodyDemo editor first** (Live Coding globally locks builds), then:
```
"E:/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" SoftBodyDemoEditor Win64 Development -Project="E:/ClaudeCode/RT_SoftBody/SoftBodyDemo.uproject" -WaitMutex
```
Open the uproject, drop a SoftBody actor, Play. Assign a **Two-Sided** material. Full details +
gotchas in `WORKFLOW.md`.

## Key Reuse Pointers (from RT_ClothSim)
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimCompute.cpp` — RDG dispatch + shader classes + colored-GS loop.
- `Plugins/ClothSim/Shaders/Private/ClothSolveGaussSeidel.usf` — per-constraint, per-color solve (= `SBSolveDistance`).
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimComponent.cpp` — `BuildConstraints` (greedy graph coloring), fixed-timestep, readback→verts, smooth normals.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothMeshSceneProxy.cpp` — FLocalVertexFactory rendering path.
- `Plugins/ClothSim/Source/ClothSim/Public/ClothSimResources.h` — params + resources + constraint structs.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimModule.cpp` — shader dir mapping.

## Inherited gotchas (apply from day one — see WORKFLOW.md)
- Smooth normals = `Cross(E2,E1)` (two-sided lighting; cloth M9).
- No `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` on the standalone RDG builder (crash; cloth M8) — use `RDG_EVENT_NAME`.
- Shader paths include the subfolder (`/SoftBodySim/Private/...`).
- Out-of-line ctor/dtor for the resources struct (TUniquePtr<FRHIGPUBufferReadback> incomplete-type; cloth C4150).
- 6-tet Kuhn split per cell (conforming) when tets arrive in SB-M2.

## Recommended Prompt For a New Claude Session
> "Read `Docs/HANDOFF.md`, `Docs/PROJECT_STATE.md`, `Docs/ARCHITECTURE.md`, and `Docs/WORKFLOW.md` to
> load context. This is a from-scratch GPU **soft body** sim in UE 5.7 (host `SoftBodyDemo`, plugin
> `SoftBodySim`), reusing the framework from the sibling cloth project at `E:\ClaudeCode\RT_ClothSim`
> (plugin `ClothSim`). M0 + SB-M1–M4 are done and verified — the core feature set (GPU tetrahedral XPBD,
> volume-preserving jelly, mouse drag, sphere/capsule + self-collision) is complete. Pick an item from
> 'Immediate Next Task — SB-M+ stretch list' in HANDOFF (or ask me which to do), following `WORKFLOW.md`:
> build via the CLI command in HANDOFF with the editor closed, then WAIT for me to verify in-editor before
> updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

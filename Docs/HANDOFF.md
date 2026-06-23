# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-23 (after SB-M3, verified in-editor; SB-M4 is the next task).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 + SB-M1 + SB-M2 + SB-M3 COMPLETE & verified in-editor.** The `SoftBodySim` plugin runs a full
  volume-preserving GPU soft body that is **interactively draggable**: a tetrahedral lattice box falls,
  squashes, bulges back to its rest volume (jelly), rests on the ground, and can be grabbed and pulled
  with the mouse. Rendered as a lit two-sided mesh. Builds clean via the CLI command below. Pushed to `main`.
- **What exists** (all in `Plugins/SoftBodySim`): plugin/module (`/SoftBodySim`→`Shaders/`),
  `SoftBodyResources.h` (params + `FGPUConstraint` + `FGPUVolumeConstraint` + `FGPUCollider` + `FSoftBodyTet`
  + resources), 6 shaders (`SBPredict`/`SBSolveDistance`/`SBSolveVolume`/**`SBGrab`**/`SBCollision`/`SBFinalize`),
  `SoftBodyCompute.cpp` (RDG: Predict → per iter {distance colors; volume colors} → [grab] → ground collision
  → Finalize → readback), `USoftBodyComponent` (centered box lattice, **6-tet Kuhn split → `Tets`**, distance
  constraints from tet edges + greedy coloring, per-tet volume constraints + tet coloring, boundary surface,
  **`BoundaryParticles` + `UpdateMouseGrab`**, fixed-step dispatch, readback→verts), `FSoftBodyMeshSceneProxy`,
  `ASoftBodyActor`. Demo: `Content/M_Softbody` (two-sided material) + a SoftBody actor in `LV_Demo`.
- **Key knobs:** `VolumeStiffness` (0 = SB-M1 flatten, 1 = jelly), `Stiffness`, `Substeps`, `SolverIterations`,
  `Anchor` (None/Top/Bottom face), `bGroundPlane`/`GroundHeight`, `bEnableMouseDrag`/`GrabStiffness`/
  `GrabPickRadiusScale`, `bShowStats`/`bDrawDebugPoints`.
- **Collisions are ground-plane only** — sphere/capsule colliders + self-collision are SB-M4.

## Immediate Next Task — SB-M4: collisions (sphere/capsule + self)
Port the remaining cloth collision passes so the jelly collides with shapes and with itself. The
`SBCollision.usf` shader **already has the full capsule routine + a world-space `FGPUCollider` buffer path**
(carried over in SB-M1) — the analytic-collider half is mostly wiring + authoring UI. Steps:
1. **Authored colliders (component):** add a `USTRUCT FSoftBodyCollider { EType Sphere/Capsule; FVector Center;
   float Radius; float HalfHeight; FRotator Rotation; }` + a `TArray<FSoftBodyCollider> Colliders` UPROPERTY
   (mirror cloth `FClothCollider`). In `TickComponent`, transform each to world space and fill
   `Params.Colliders` (sphere → A==B; capsule → A/B = center ± axis*halfHeight), exactly like
   `UClothSimComponent::TickComponent`. The dispatch already binds `Params.Colliders` and passes `NumColliders`
   to `SBCollision.usf`, so this should light up with no shader change. Add optional `DrawDebug` wireframes.
2. **Self-collision (port cloth):** copy `ClothBuildGrid.usf` + `ClothSelfCollision.usf` → `SBBuildGrid.usf` +
   `SBSelfCollision.usf` (spatial-hash broadphase + Jacobi repulsion). Add `FSBBuildGridCS`/`FSBSelfCollisionCS`
   classes (copy from `ClothSimCompute.cpp`, incl. the `NextPrime`/`kMaxPerCell` helpers) and the per-substep
   build+respond passes (after the solve, before/with collision), ping-ponging a scratch buffer. NOTE: cloth's
   self-collision excludes *grid* 1-ring neighbours; for the soft body the natural exclusion is **lattice
   1-ring** (|dx|,|dy|,|dz| ≤ 1) — adapt the neighbour test. Gate behind a `bSelfCollision` UPROPERTY +
   `SelfThickness`/`SelfStiffness`/`SelfIterations` params (mirror cloth).
3. Reuse `Params.Friction`, the dummy-collider-when-ground-only path, and the `kThreadGroupSize`/`NextPrime`
   sizing already proven in cloth.
- **Verify:** drop the jelly onto a sphere/capsule (it drapes/squashes around it); enable self-collision and
  fold/compress it so layers don't interpenetrate. **Do not commit/update docs until the user confirms in-editor.**
- After SB-M4 the core feature set (Obi/Zibra-style) is complete; remaining work is the SB-M+ stretch list.

## Cloth reuse pointers for SB-M4 (from RT_ClothSim)
- `Plugins/ClothSim/Shaders/Private/ClothCollision.usf` — already ported as `SBCollision.usf` (capsule + ground).
- `Plugins/ClothSim/Shaders/Private/ClothBuildGrid.usf`, `ClothSelfCollision.usf` — copy → `SB*` (spatial-hash self-collision).
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimCompute.cpp` — `FClothBuildGridCS`/`FClothSelfCollisionCS` classes, `NextPrime`, `kMaxPerCell`, and the build+respond dispatch loop to copy.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimComponent.cpp` — `FClothCollider` struct, the world-space collider build in `TickComponent`, and `DrawColliders`.

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
> (plugin `ClothSim`). M0 + SB-M1 (GPU lattice solid) + SB-M2 (volume-preserving jelly) + SB-M3 (mouse
> dragging) are done and verified. Implement the milestone under 'Immediate Next Task' (SB-M4: sphere/capsule
> colliders + spatial-hash self-collision), following `WORKFLOW.md`: build via the CLI command in HANDOFF
> with the editor closed, then WAIT for me to verify in-editor before updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-22 (after SB-M2, verified in-editor; SB-M3 is the next task).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 + SB-M1 + SB-M2 COMPLETE & verified in-editor.** The `SoftBodySim` plugin runs a full
  volume-preserving GPU soft body: a tetrahedral lattice box falls, squashes, and **bulges back to its
  rest volume** (jelly), resting on the built-in ground plane; rendered as a lit two-sided mesh. Builds
  clean via the CLI command below. Pushed to `main`.
- **What exists** (all in `Plugins/SoftBodySim`): plugin/module (`/SoftBodySim`→`Shaders/`),
  `SoftBodyResources.h` (params + `FGPUConstraint` + `FGPUVolumeConstraint` + `FSoftBodyTet` + resources),
  5 shaders (`SBPredict`/`SBSolveDistance`/**`SBSolveVolume`**/`SBCollision`/`SBFinalize`),
  `SoftBodyCompute.cpp` (RDG: Predict → per iter {distance colors; volume colors} → ground collision →
  Finalize → readback), `USoftBodyComponent` (centered box lattice, **6-tet Kuhn split → `Tets`**, distance
  constraints from tet edges + greedy coloring, **per-tet volume constraints + tet coloring**, boundary
  surface, fixed-step dispatch, readback→verts), `FSoftBodyMeshSceneProxy`, `ASoftBodyActor`.
  Demo: `Content/M_Softbody` (two-sided material) + a SoftBody actor in `LV_Demo`.
- **Key knobs:** `VolumeStiffness` (0 = SB-M1 flatten, 1 = jelly), `Stiffness`, `Substeps`, `SolverIterations`,
  `Anchor` (None/Top/Bottom face), `bGroundPlane`/`GroundHeight`, `bShowStats`/`bDrawDebugPoints`.
- **No mouse interaction yet** — that's SB-M3.

## Immediate Next Task — SB-M3: mouse dragging
Let the user poke/pull/stretch the body with the cursor. Reuses the existing position readback + per-frame
param-push pattern; **no extra GPU readback needed**. Suggested approach:
1. **Pick (game thread, on click):** `APlayerController::DeprojectMousePositionToWorld` → world ray. Using
   the latest `RenderResources->PositionCopy` (world-space, guarded by `PositionCopyCS`), find the nearest
   **boundary** particle to the ray (smallest point-to-ray distance). Remember its particle index + the grab
   depth (distance along the ray to that particle). Store on the component.
2. **Drag (each frame while held):** target world pos = ray origin + dir * grabDepth (recompute the ray from
   the current cursor). Add to `FSoftBodyParams` a `bool bGrabActive; int32 GrabIndex; FVector3f GrabTarget;
   float GrabStiffness;` and push them in `TickComponent`.
3. **Grab solve (GPU):** add a tiny pass (or fold into Finalize/Predict) that pulls particle `GrabIndex`
   toward `GrabTarget`: `p += GrabStiffness * (GrabTarget - p)` on the predicted buffer (a soft attachment),
   or temporarily set its InvMass to 0 and hard-set its predicted position to the target (firm pin). A new
   `SBGrab.usf` single-thread dispatch keyed on `GrabIndex` is cleanest; bind it after the constraint solve,
   before collision, so the grabbed vertex still respects the ground.
4. **Input:** simplest is to poll in `TickComponent` via the owning `APlayerController` (mouse button +
   `DeprojectMousePositionToWorld`), so no input-binding setup is needed in the demo map. Enable the cursor.
- **Verify:** click-drag a face/corner and the jelly follows the cursor, stretching and wobbling; release and
  it springs back. **Do not commit/update docs until the user confirms in-editor.**
- Then SB-M4 (sphere/capsule colliders + spatial-hash self-collision — port the cloth passes). See `ROADMAP.md`.

## Inherited gotchas confirmed so far
- A `TArray<T>` **value member** in a `UCLASS` header needs the COMPLETE type `T` (not a forward declaration)
  or the UHT-generated `.gen.cpp` destructor fails with incomplete-type C2672. The component stores
  `TArray<FSoftBodyTet>`, so `SoftBodyComponent.h` includes `SoftBodyResources.h` (where the volume structs live too).
- GPU constraint structs use a **tight scalar layout** that must match between C++ and HLSL (e.g.
  `FGPUVolumeConstraint` = 4×uint + 2×float = 24 B). Structured buffers pack tightly; the 16-byte-alignment
  trap is constant-buffer-only. `Cross(E2,E1)` normals + two-sided material; `RDG_EVENT_NAME` only (no breadcrumb scopes).

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
> (plugin `ClothSim`). M0 + SB-M1 (GPU lattice solid) + SB-M2 (volume-preserving jelly) are done and
> verified. Implement the milestone under 'Immediate Next Task' (SB-M3: mouse dragging), following
> `WORKFLOW.md`: build via the CLI command in HANDOFF with the editor closed, then WAIT for me to verify
> in-editor before updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

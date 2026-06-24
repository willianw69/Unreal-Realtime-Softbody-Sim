# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-24 (after SB-M9, verified; multi-body collision via a world subsystem).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 + SB-M1–M9 COMPLETE & verified in-editor.** The `SoftBodySim` plugin runs a full volume-preserving
  GPU soft body: jelly that falls/squashes/bulges back, rests on the ground, collides with sphere/capsule
  shapes, arbitrary scene meshes (GDF), itself, and **other soft body actors** (SB-M9); is mouse-draggable;
  can simulate **any assigned Static Mesh** via an FFD cage (SB-M5) with **vertex-color weight-painted
  stiffness** (SB-M6) made robust by an **XPBD distance solve** (SB-M7). Builds clean via the CLI command
  below. Pushed to `main`.
- **What exists** (all in `Plugins/SoftBodySim`): plugin/module (`/SoftBodySim`→`Shaders/`),
  `SoftBodyResources.h` (params + structs + resources + compute decls), **11 shaders** (`SBPredict`/
  `SBSolveDistance`/`SBSolveDistanceXPBD`/`SBSolveVolume`/`SBGrab`/`SBBuildGrid`/`SBSelfCollision`/
  **`SBInterBodyCollide`**/`SBCollision`/`SBCollisionDF`/`SBFinalize`), `SoftBodyCompute.cpp` (per-body RDG
  pipeline + `DispatchInterBody_RenderThread`), `FSoftBodySceneViewExtension` (GDF snapshot),
  **`USoftBodyWorldSubsystem`** (`SoftBodyWorldSubsystem.h/.cpp` — registers bodies, runs the inter-body pass),
  `USoftBodyComponent` (cage/mesh-fit cage, tets, constraints, embedding, weight sampling, grab, all collision
  modes incl. inter-body opt-in, dynamic bounds, readback→verts; exposes `GetRenderResources()`),
  `FSoftBodyMeshSceneProxy`, `ASoftBodyActor`. Demo: `Content/M_Softbody`.
- **Per-substep pipeline (per body):** `SBPredict → [distance (XPBD|PBD); volume]×iters → [self-collision
  ×iters] → [SBGrab] → SBCollision (shapes+ground) → [SBCollisionDF] → SBFinalize → readback`. **Then once
  per frame (subsystem, after all bodies):** the inter-body shared-grid repulsion correction.
- **Key knobs:** earlier ones **plus** `bInterBodyCollision`/`InterBodyThickness`/`InterBodyStiffness`/
  `InterBodyIterations` (SB-M9). Cage `ResX/Y/Z` cap 64. `r.GenerateMeshDistanceFields=True` set in `DefaultEngine.ini`.
- **DEMO ASSET NOTE:** in-editor tests used large AI-generated meshes (~50–57 MB: `tripo_convert_*`,
  `MilkDragons`, `*_3d_model_*` + textures) that are **deliberately NOT committed** (repo size, no LFS,
  third-party provenance) — see `.gitignore`. The committed `LV_Demo` uses the box soft body; `LV_Demo.umap`
  stays modified locally with the test setup but isn't committed. To reproduce: assign any Static Mesh to
  `SoftBody → SourceMesh` (engine `/Engine/BasicShapes/Sphere` works), give it vertex colors for weight paint,
  and drop DF-enabled static meshes nearby for DF collision.

## Immediate Next Task — none mandatory; SB-M+ stretch list
Every planned milestone + the user's mesh / weight-paint / arbitrary-mesh-collider / multi-body requests are
delivered and verified. No milestone is in progress. Optional next steps, in rough value order — confirm with
the user which (if any) to pursue, then follow `WORKFLOW.md` (build with editor closed → verify → docs/commit):
1. **Profiling pass** — `stat GPU` / Unreal Insights at a few cage resolutions / body counts; record per-pass
   cost in DEVLOG. Cheap, high-signal for the portfolio. Good first pick.
2. **Higher-fidelity colliders** — the SB-M8 GDF is coarse for thin/small meshes. For a specific collider, bake
   a custom SDF volume (or sample a per-mesh DF) for sharp contact. Overlaps with #3.
3. **Non-box / conforming cage** — SDF-voxelize the assigned mesh to keep only cage cells inside it, so the
   cage hugs the shape (tighter sim than the current bounding-box FFD cage).
4. **XPBD volume solve** — extend XPBD (done for distance, SB-M7) to the volume constraint (note: volume
   compliance has different units — scale per tet rest-volume).
5. **Render-mesh skinning upgrade** — smooth/multi-tet barycentric bind (current SB-M5 binds each vert to one
   tet); inter-body broadphase for many bodies (SB-M9 has none yet); zero-copy GPU vertex write.
- The cloth project (`RT_ClothSim`) has reference implementations/habits for profiling.

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
- Changing a GPU constraint struct's size means updating EVERY HLSL `struct` that reads that buffer
  (SB-M7 grew `FGPUConstraint` 16→20 B for `Softness` → both `SBSolveDistance.usf` and `SBSolveDistanceXPBD.usf`).
- To clear an RDG **structured/typed float buffer** to 0 without depending on a float-clear overload: clear
  it through a `PF_R32_UINT` UAV view to `0u` (0.0f's bit pattern is 0), and bind a `PF_R32_FLOAT` UAV for use
  (XPBD λ reset, SB-M7).
- Reading a `UStaticMesh` on the CPU (`GetRenderData()->LODResources[0]` positions/indices/UVs/colors) works
  in-editor; packaged builds need **Allow CPUAccess** on the asset (SB-M5).

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
> (plugin `ClothSim`). M0 + SB-M1–M9 are done and verified — GPU tetrahedral sim, volume-preserving jelly,
> mouse drag, sphere/capsule + self + Global-Distance-Field + multi-body collision, custom-mesh FFD embedding,
> vertex-color weight-painted stiffness, and an XPBD distance solve. Pick an item from 'Immediate Next Task
> — SB-M+ stretch list' in HANDOFF (or ask me which to do), following `WORKFLOW.md`:
> build via the CLI command in HANDOFF with the editor closed, then WAIT for me to verify in-editor before
> updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

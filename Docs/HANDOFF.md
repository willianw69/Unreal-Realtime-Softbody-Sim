# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-22 (after M0 bootstrap; SB-M1 is the next task).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 bootstrap COMPLETE:** `SoftBodyDemo` C++ host project builds clean via the CLI. No plugin yet.
- Full design + docs written (`Docs/`). Method chosen: **tetrahedral XPBD** (user-approved).
- **Nothing simulated yet** — SB-M1 creates the `SoftBodySim` plugin and the first working sim.

## Immediate Next Task — SB-M1: GPU lattice solid
Create `RT_SoftBody/Plugins/SoftBodySim` by porting the ClothSim GPU framework, then:
1. **Plugin scaffolding:** `SoftBodySim.uplugin`, `Source/SoftBodySim/SoftBodySim.Build.cs` (deps:
   Core, CoreUObject, Engine, RenderCore, RHI, Renderer, Projects — mirror `ClothSim.Build.cs`),
   `Private/SoftBodySimModule.cpp` mapping `/SoftBodySim` → `Shaders/`. Enable in `SoftBodyDemo.uproject`.
2. **Resources** (`Public/SoftBodyResources.h`): port `FClothSimParams`/`FClothRenderResources` →
   `FSoftBodyParams`/`FSoftBodyRenderResources` (pooled Positions/Velocities/InvMass, readback,
   distance-constraint buffer + color ranges).
3. **Component** (`SoftBodyComponent.*`, a `UMeshComponent`): runtime-generate a box **lattice**
   (`Res.x×Res.y×Res.z`, `Spacing`), **distance constraints** (unique tet/lattice edges), and **greedy
   graph coloring** (port cloth `BuildConstraints`). Fixed-timestep accumulator; push params; read
   back positions → boundary surface verts + CPU normals (`Cross(E2,E1)`). Add a built-in ground
   plane + an anchor option (pin one face) for the first test.
4. **Compute** (`SoftBodyCompute.cpp`): port the RDG dispatch + shader classes; pipeline
   `SBPredict → colored-GS distance solve → SBFinalize → readback`.
5. **Shaders** (`Shaders/Private/`): `SBPredict.usf` (gravity+damping), `SBSolveDistance.usf`
   (= cloth `ClothSolveGaussSeidel.usf`), `SBFinalize.usf`, `SBCollision.usf` (ground plane).
6. **Proxy** (`SoftBodyMeshProxy.*`): port `ClothMeshSceneProxy` (FLocalVertexFactory surface mesh).
- **Verify:** a lattice box falls, jiggles/sags, and rests on the ground (may lose volume — that's
  expected until SB-M2). See `WORKFLOW.md` for the build/run/verify loop. **Do not commit/update docs
  until the user confirms in-editor.**
- Then proceed SB-M2 (volume constraints → jelly), SB-M3 (mouse drag), SB-M4 (collisions). See `ROADMAP.md`.

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
> `SoftBodySim` to be created), reusing the framework from the sibling cloth project at
> `E:\ClaudeCode\RT_ClothSim` (plugin `ClothSim`). M0 (host build) is done. Implement the milestone
> under 'Immediate Next Task' (SB-M1), following `WORKFLOW.md`: build via the CLI command in HANDOFF
> with the editor closed, then WAIT for me to verify in-editor before updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

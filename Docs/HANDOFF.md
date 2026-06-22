# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-22 (after SB-M1, verified in-editor; SB-M2 is the next task).

## Project Summary
From-scratch **GPU soft body simulation in UE 5.7** (no Chaos). Custom compute shaders do
**tetrahedral XPBD/PBD** (distance + per-tet **volume** constraints → volume-preserving "jelly",
Obi/Zibra-style) on structured buffers; rendered as a dynamic lit mesh; **mouse-draggable**.
Host project `SoftBodyDemo`, all real work in `Plugins/SoftBodySim`. Engine: `E:\Epic Games\UE_5.7`.
**This project deliberately reuses the framework from the sibling project `E:\ClaudeCode\RT_ClothSim`
(plugin `ClothSim`, M1–M9 complete)** — copy + adapt its files (see the reuse map in `ARCHITECTURE.md`).

## Current State
- **M0 + SB-M1 COMPLETE & verified in-editor.** The `SoftBodySim` plugin exists and runs: a box lattice
  falls, jiggles/sags via GPU distance constraints, and rests on the built-in ground plane; rendered as a
  lit two-sided mesh. Builds clean via the CLI command below. Pushed to `main`.
- **What SB-M1 built** (all in `Plugins/SoftBodySim`): plugin/module (`/SoftBodySim`→`Shaders/`),
  `SoftBodyResources.h`, 4 shaders (`SBPredict`/`SBSolveDistance`/`SBCollision`/`SBFinalize`),
  `SoftBodyCompute.cpp` (RDG: Predict → colored-GS distance solve → ground collision → Finalize → readback),
  `USoftBodyComponent` (centered box lattice, **6-tet Kuhn split → `Tets`**, deduped distance constraints
  from tet edges + greedy coloring, boundary surface, fixed-step dispatch, readback→verts), `FSoftBodyMeshSceneProxy`,
  `ASoftBodyActor`. Demo: `Content/M_Softbody` (two-sided material) + a SoftBody actor placed in `LV_Demo`.
- **Volume NOT preserved yet** — the box can flatten/lose volume on impact. That's exactly what SB-M2 fixes.

## Immediate Next Task — SB-M2: volume constraints → jelly
The headline feature. Add per-tetrahedron **volume constraints** as a **second graph-colored constraint set**
over the `Tets` array **already built in SB-M1** (`USoftBodyComponent::BuildTets`). Steps:
1. **Resources** (`SoftBodyResources.h`): add a `FGPUVolumeConstraint { uint4 idx; float RestVol; float Stiff; }`
   (match a 32-byte HLSL struct), a second pooled `VolumeConstraintsBuffer` + its own `TArray<FSoftBodyColorRange>
   VolumeColorRanges` + `NumVolumeConstraints` on `FSoftBodyRenderResources`. Add a `VolumeStiffness` param to
   `FSoftBodyParams` + a `UPROPERTY` on the component.
2. **Build** (component): for each tet compute `V0 = (1/6)·dot(e1, e2×e3)` with `e_k = p_k − p_0` from rest
   positions; **graph-color the tets** (two tets conflict if they share ANY of their 4 vertices — same greedy
   scheme as edges but 4 indices per constraint). Sort into a color-ranged buffer; upload in `InitResources`.
3. **Shader** `SBSolveVolume.usf`: one thread per tet per color. `C = V − V0`; per-vertex gradients
   `g0 = (e3−e1)×(e2−e1)`-style cross products of opposing edges (g1=e2×e3, g2=e3×e1, g3=e1×e2, g0=−(g1+g2+g3),
   each ×1/6); `λ = −C / Σ w_k|g_k|²`; `p_k += λ·w_k·g_k·(Stiffness·VolumeStiffness)`. Race-free within a color.
4. **Dispatch** (`SoftBodyCompute.cpp`): in the substep loop, after the distance-color loop, add a
   volume-color loop (same in-place pattern on `Predicted`): `for iter { for distColors; for volColors }`
   (or interleave). Bind `VolumeConstraintsBuffer` SRV.
- **Verify:** the box now keeps its volume — squashes and bulges back like jelly instead of collapsing flat.
  Tune distance-vs-volume relative stiffness + iterations. **Do not commit/update docs until the user confirms
  in-editor.**
- Then proceed SB-M3 (mouse drag), SB-M4 (sphere/capsule + self-collision). See `ROADMAP.md`.

## Inherited gotcha confirmed in SB-M1
- A `TArray<T>` **value member** in a `UCLASS` header needs the COMPLETE type `T` (not a forward declaration)
  or the UHT-generated `.gen.cpp` destructor fails with incomplete-type C2672. SB-M1 stores `TArray<FSoftBodyTet>`,
  so `SoftBodyComponent.h` includes `SoftBodyResources.h`. SB-M2's volume-constraint structs live there too.

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
> (plugin `ClothSim`). M0 + SB-M1 (GPU lattice solid with distance constraints) are done and verified.
> Implement the milestone under 'Immediate Next Task' (SB-M2: per-tet volume constraints → jelly),
> following `WORKFLOW.md`: build via the CLI command in HANDOFF with the editor closed, then WAIT for
> me to verify in-editor before updating docs or committing.
> Update PROJECT_STATE/ROADMAP/DEVLOG/HANDOFF/PORTFOLIO_NOTES (+ARCHITECTURE if it changes) per
> milestone. Port/adapt cloth files per the reuse map in ARCHITECTURE.md; apply the inherited
> gotchas (Cross(E2,E1) normals; no RHI breadcrumb scopes) from the start. Git remote:
> https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git (push main per milestone, after I verify)."

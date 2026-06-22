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

<!-- Append SB-M3, SB-M4, … entries below after each milestone is implemented + verified in-editor. -->

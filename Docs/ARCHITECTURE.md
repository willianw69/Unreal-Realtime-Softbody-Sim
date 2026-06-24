# ARCHITECTURE.md

> Technical system documentation. Update whenever the architecture changes.
> Last updated: 2026-06-24 (SB-M1–M9 — adds multi-body collision via a world subsystem + shared spatial hash).
> Reference implementation for the reused framework: `E:\ClaudeCode\RT_ClothSim` (plugin `ClothSim`).

**Status note (through SB-M7):** the SB-M1–M4 core (lattice, 6-tet Kuhn split, distance + per-tet volume
constraints, mouse-drag grab, sphere/capsule + ground collision, spatial-hash self-collision, boundary-surface
render) is all implemented, plus three additions:
- **Custom mesh embedding (SB-M5):** assign a `UStaticMesh` → the lattice becomes a box **cage** auto-fit to
  the mesh bounds; each mesh vertex is barycentric-bound to its containing cage tet (`BuildEmbedding`) and
  reconstructed from the deformed cage each frame. The sim still runs on the cage; only render-vertex sourcing
  changed. No mesh → the default box boundary surface.
- **Weight-painted stiffness (SB-M6):** the mesh's vertex-color R channel → per-cage-particle weight
  (`BuildParticleWeights`) → per-constraint softness.
- **XPBD distance solve (SB-M7):** `SBSolveDistanceXPBD.usf` with per-constraint compliance + a λ accumulator
  (reset per substep); `bUseXPBD` toggle; compliance 0 ≡ stiff PBD. Volume solve stays PBD.
- **Distance-field collision (SB-M8):** `FSoftBodySceneViewExtension` snapshots the scene Global Distance
  Field each frame (render-thread cache `SoftBodyGDF::Get`); `SBCollisionDF.usf` (`FSBCollisionDFCS`) samples
  it per particle and projects out, additive with the analytic colliders. Needs `r.GenerateMeshDistanceFields`.
- **Dynamic render bounds (SB-M8 fix):** the component recomputes `LocalBounds` from the deformed verts each
  frame and pushes them to the render thread (`UpdateBounds` + `MarkRenderTransformDirty`) so the body isn't
  frustum-culled when it travels far from the actor.
- **Multi-body collision (SB-M9):** `USoftBodyWorldSubsystem` registers all components and, after they sim,
  enqueues `SoftBodyCompute::DispatchInterBody_RenderThread` — concatenate every opted-in body's Positions/
  InvMasses + body-id into combined buffers, build a shared hash grid (`SBBuildGrid.usf`), repel different-body
  particles (`SBInterBodyCollide.usf`), copy corrections back to each body's Positions. A post-sim positional
  projection; per-body substeps untouched.

Actual per-substep pipeline:
`SBPredict → [colored-GS distance (XPBD or PBD) + colored-GS volume]×iters → [self-collision build+respond
×iters, ping-pong] → [SBGrab] → SBCollision (shapes + ground) → [SBCollisionDF] → SBFinalize → readback`.
The "PredictedA/B ping-pong" row below is real: the colored solves/grab/collision write in place on
`PredictedA`, and self-collision ping-pongs `PredictedA`↔`PredictedB` (a `Solved` ref tracks the current
buffer). Remaining items are the SB-M+ stretch list (per-mesh/custom SDF colliders, conforming cage, XPBD
volume, render-mesh skinning upgrade, multiple bodies, profiling).

## High-Level Architecture (intended, mirrors ClothSim)

```
                       GAME THREAD                          RENDER THREAD (GPU)
  ┌─────────────────────────────────────────┐   ┌────────────────────────────────────┐
  │ USoftBodyComponent (UMeshComponent)      │   │ SoftBodyCompute (RDG passes)         │
  │  - sim params (lattice res, gravity)     │   │                                      │
  │  - fixed-timestep accumulator            │   │  Persistent pooled buffers:          │
  │  - builds lattice + tets + constraints   │──▶│   Positions, Velocities, InvMass,    │
  │    + graph coloring (RUNTIME)            │   │   Constraints (distance, volume)     │
  │  - per fixed step: ENQUEUE dispatch ──────┼──▶│                                      │
  │  - mouse pick + grab target (SB-M3)       │   │  Per frame / substep:                │
  │  - reads readback → boundary verts+normals│   │   Predict → Solve(dist+vol) →        │
  │  - pushes verts to proxy ─────────────────┼──▶│   [Collision] → Finalize → readback  │
  └─────────────────────────────────────────┘   └────────────────┬─────────────────────┘
                     │ CreateSceneProxy                            │ positions
                     ▼                                             ▼
            ┌──────────────────────┐                   ┌────────────────────────┐
            │ FSoftBodyMeshProxy   │   lock+memcpy      │ FRHIGPUBufferReadback  │
            │ FLocalVertexFactory  │◀──────────────────│ (non-stalling)          │
            │ + index/UV/color VBs │   verts each frame └────────────────────────┘
            └──────────────────────┘
                     │ standard mesh draw  →  lit soft body on screen
```

## Reuse map (ClothSim → SoftBodySim)
The soft body **ports + adapts** the cloth GPU framework (separate project = copy, not share).
~70–80% transfers. The cloth's colored Gauss-Seidel solver is the key reuse — a volume constraint
is just another constraint type with its own colors and per-color dispatch.

| Cloth (RT_ClothSim) | Soft body (RT_SoftBody) | Reuse |
|---|---|---|
| `ClothSim.uplugin`, `ClothSimModule.cpp` | `SoftBodySim.uplugin`, `SoftBodySimModule.cpp` | rename; shader path `/SoftBodySim` → `Shaders/` |
| `ClothSimResources.h` | `SoftBodyResources.h` | pooled Positions/Velocities/InvMass + readback + constraint buffer + color ranges; add tet structs |
| `ClothSimCompute.cpp` | `SoftBodyCompute.cpp` | RDG dispatch, shader classes, `InitResources`, colored-GS loop; swap pass set |
| `ClothPredict.usf` | `SBPredict.usf` | gravity + damping + predicted position; drop wind/normals |
| `ClothSolveGaussSeidel.usf` | `SBSolveDistance.usf` | distance constraint, 1 thread/constraint per color — keep |
| `ClothFinalize.usf` | `SBFinalize.usf` | `v=(p−x)/dt; x=p` — keep |
| `ClothCollision.usf` | `SBCollision.usf` | ground/sphere + ground plane — reuse (SB-M4) |
| `ClothBuildGrid/SelfCollision.usf` | `SBBuildGrid/SelfCollision.usf` | spatial-hash self-collision — reuse (SB-M4) |
| `ClothMeshSceneProxy.*` | `SoftBodyMeshProxy.*` | FLocalVertexFactory + per-frame vert/normal upload — keep |
| `ClothSimComponent.*` | `SoftBodyComponent.*` | UMeshComponent, fixed-timestep, param push, readback→verts, greedy coloring — keep; replace topology |

## Tetrahedral model
### Lattice (runtime, game thread)
`Res.x × Res.y × Res.z` particles, `Spacing` apart; index `i = x + y*Rx + z*Rx*Ry`. Shape = Box
(SB-M1); Sphere-SDF mask later. Each cube cell → **6 conforming tetrahedra** (fixed main-diagonal
Kuhn split — always face-conforming so adjacent cells share faces without cracks).

### Constraints (runtime, graph-colored)
- **Distance** (SB-M1): every unique tet edge (deduped); rest length from rest positions →
  `SBSolveDistance.usf`. Resists stretch.
- **Volume** (SB-M2): per tetrahedron, rest volume `V0 = (1/6)·dot(e1, e2×e3)` with `e_k = p_k − p_0`.
  PBD: `C = V − V0`; per-vertex gradients are cross products of opposing edges; move all 4 vertices
  mass-weighted → **new `SBSolveVolume.usf`**. Preserves volume = jelly.
- Two constraint **sets** (edges, tets), each greedily colored (no two constraints in a color share a
  particle → race-free in-place per-color dispatch, RDG serializes colors). Reuses the cloth
  `BuildConstraints` coloring. Each set stores `[Start,Count)` color ranges in the resources struct.

### Solve pipeline (per substep, RDG)
`SBPredict → for SolverIterations: { distance colors; volume colors } → [Collision] → SBFinalize →
EnqueueCopy(readback)`. Velocity derived from position delta in Finalize (PBD stability).

## GPU Buffers (intended)
| Buffer | Type | Lifetime | Notes |
|---|---|---|---|
| Positions | StructuredBuffer<float3> | persistent (pooled) | world space |
| Velocities | StructuredBuffer<float3> | persistent (pooled) | cm/s |
| InvMasses | StructuredBuffer<float> | persistent (pooled) | 0 = pinned/anchored |
| PredictedA/B | StructuredBuffer<float3> | transient | solver ping-pong (distance); volume solves in place |
| DistanceConstraints | StructuredBuffer<{uint A,uint B,float Rest,float Stiff}> | persistent | color-sorted |
| VolumeConstraints (SB-M2) | StructuredBuffer<{uint4 idx, float RestVol, float Stiff}> | persistent | color-sorted |
| PositionReadback | FRHIGPUBufferReadback | persistent | non-stalling CPU copy for render + mouse pick |

## Rendering
Static **boundary surface** built once: for a box, the 6 outer faces, each an (Ra×Rb) grid
triangulated like the cloth (index buffer + UVs). Surface verts map to lattice particle indices.
Each frame: readback positions → boundary verts → CPU smooth area-weighted normals/tangents →
`FSoftBodyMeshProxy` lock+memcpy upload. **Use `Cross(E2,E1)`** for normals (agrees with UE's
left-handed winding so two-sided materials light both faces — direct lesson from cloth M9). General
boundary-from-tets extraction (faces used by exactly one tet) arrives with non-box shapes.

## Mouse dragging (SB-M3)
On click: `DeprojectMousePositionToWorld` → world ray; pick the nearest **boundary** particle (CPU,
from the readback) and remember its index + grab depth. Each frame: target world pos = ray point at
grab depth; push `{grabbedIndex, targetWorldPos}` to the render thread; a grab step pulls that
particle toward the target (soft attachment, or temporary pin). Reuses the readback + param-push
pattern; no extra GPU readback needed.

## Threading / responsibilities (mirrors cloth)
- **Game thread:** params, lattice + tet + constraint construction + coloring (runtime), fixed-step
  accumulation, mouse pick, readback → boundary verts + normals, proxy update enqueue.
- **Render thread:** all simulation math (predict/solve/finalize/collision), the readback copy.

## Hard-won rules inherited from ClothSim (apply from day one)
- **Normals:** `Cross(E2,E1)` (not `E1,E2`) for two-sided lighting (cloth M9).
- **Do NOT** put `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` on this plugin's standalone `FRDGBuilder`
  (run from a render command) — RHI breadcrumb imbalance → crash at `Execute()` (cloth M8). Use
  per-pass `RDG_EVENT_NAME` labels for profiling instead.
- `float3` structured buffers use a tight 12-byte stride (the 16-byte alignment trap is for constant
  buffers, not structured buffers).
- Atomics only where unavoidable (e.g. spatial-hash build); keep the constraint response race-free
  via graph coloring.

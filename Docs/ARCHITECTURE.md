# ARCHITECTURE.md

> Technical system documentation. Update whenever the architecture changes.
> Last updated: 2026-06-22 (SB-M1 implemented + verified — the pipeline below is now real, minus the
> volume constraint set, which is SB-M2).
> Reference implementation for the reused framework: `E:\ClaudeCode\RT_ClothSim` (plugin `ClothSim`).

**SB-M1 status note:** everything below is implemented EXCEPT the volume constraint set. The lattice,
the **6-tet Kuhn decomposition** (`USoftBodyComponent::BuildTets`), the distance constraints derived
from unique tet edges + graph coloring, the boundary surface, the `SBPredict → colored-GS
SBSolveDistance → SBCollision (ground) → SBFinalize → readback` pipeline, and the `FSoftBodyMeshSceneProxy`
render path all exist and run. SB-M2 adds the second (volume) constraint set over the already-built
`Tets` array; the "PredictedA/B ping-pong" row below is currently a single in-place `Predicted` buffer
(colored GS + collision both solve in place, so no ping-pong is needed yet).

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

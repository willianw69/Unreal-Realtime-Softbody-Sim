# PORTFOLIO_NOTES.md

> Portfolio-worthy accomplishments per milestone. Source material for portfolio breakdowns,
> resume bullets, and interview prep. Fill in per milestone after it's verified.

---

## Project pitch
A real-time **GPU soft body simulator built from scratch** in Unreal Engine 5.7 — **no Chaos** —
using custom HLSL compute shaders and the Render Dependency Graph. Tetrahedral **XPBD** with
runtime-generated distance + **volume** constraints gives volume-preserving "jelly" (à la Obi /
Zibra), rendered as a dynamic lit mesh and interactively **draggable** with the mouse. Built on the
same from-scratch GPU framework as the sibling cloth simulator, demonstrating that the architecture
generalizes across simulation types.

## Planned talking points (confirm/expand as milestones land)
- **Tetrahedral XPBD on the GPU:** runtime-generate a particle lattice + tetrahedra; solve distance
  and per-tet **volume** constraints. Volume preservation is the defining "jelly" behaviour.
- **Graph-colored Gauss-Seidel reused across sims:** the cloth solver's coloring framework drives
  the soft body too — each constraint type (distance, volume) is a color set with race-free,
  in-place, per-color GPU dispatches.
- **Framework reuse / generalization:** ~70–80% of the cloth GPU framework (RDG dispatch, pooled
  buffers, predict/finalize, readback rendering, colored solver, collisions) transferred to a new
  simulation domain — a strong systems-design signal.
- **Interactive GPU sim:** mouse picking against GPU-driven geometry (via the readback) + a grab
  constraint, so the body can be poked, pulled, and stretched in real time.
- **No-Chaos, RHI-level integration:** everything runs through UE 5.7's RHI/RDG with custom compute
  shaders and a custom `FPrimitiveSceneProxy` — no engine physics dependency.

## Resume bullet candidates (draft)
- "Built a real-time GPU soft body simulation in Unreal Engine 5.7 from scratch (no Chaos) using
  custom HLSL compute shaders and RDG — tetrahedral XPBD with runtime-generated distance and volume
  constraints for volume-preserving deformation."
- "Generalized a from-scratch GPU cloth framework to soft bodies, reusing a graph-colored
  Gauss-Seidel constraint solver across both simulation types."
- "Implemented interactive mouse dragging of GPU-simulated geometry via screen-space picking and a
  grab constraint."

## Interview discussion points
- Why tetrahedral XPBD with volume constraints over pressure/shape-matching for solid jelly.
- How graph coloring makes a sequential Gauss-Seidel solve safe AND parallel on the GPU.
- Reusing one GPU simulation framework across cloth and soft body — what transferred, what didn't.
- CPU/GPU threading model in UE (game vs render thread, RDG, pooled vs transient buffers, readback).

---

## SB-M1 — GPU lattice solid (framework port) ✅ 2026-06-22
**Shipped:** A from-scratch GPU soft body plugin (`SoftBodySim`) in UE 5.7, running the full pipeline
end-to-end: a runtime-generated 3D particle lattice is decomposed into tetrahedra (6-tet Kuhn split),
distance constraints are derived from the unique tet edges and graph-colored, solved on the GPU with a
colored Gauss-Seidel relaxation (compute shaders + RDG), collided against a ground plane, and rendered
as a dynamic lit mesh via a custom `FPrimitiveSceneProxy`. A box drops, jiggles, and settles on the floor.

**Talking points proven in M1:**
- **Framework generalization, concretely demonstrated:** ~75% of the from-scratch *cloth* GPU framework
  (RDG dispatch + shader-class boilerplate, pooled/transient buffer split, predict/finalize, colored
  Gauss-Seidel solver, non-stalling readback rendering, `FLocalVertexFactory` proxy) transferred to a new
  3D simulation domain with the solver math unchanged — the constraint solver is topology-agnostic, so a
  tetrahedral lattice's edges relax exactly like a cloth grid's. Strong systems-design / reuse signal.
- **Runtime topology generation:** lattice → 6-tet Kuhn/Freudenthal decomposition (face-conforming by
  sharing a consistent cell diagonal) → edge dedup via 64-bit (min,max) keys → greedy graph coloring so
  each color is a race-free, in-place, per-color GPU dispatch.
- **Boundary-surface rendering with correct two-sided lighting:** per-quad winding chosen so the
  `Cross(E2,E1)` smooth normals face outward, matching UE's left-handed front-face convention.
- **GPU/CPU threading discipline in UE:** game thread builds topology + pushes a param snapshot; render
  thread owns all sim state (pooled buffers registered into a fresh RDG each frame) and runs the compute
  passes; a fenced, non-stalling readback feeds the render verts.

**Engineering war story (interview-ready):** a `TArray<FSoftBodyTet>` value member with only a
forward-declared element type compiled fine in isolation but broke in the UHT-generated `.gen.cpp`
destructor (incomplete-type, C2672) — a concrete lesson in how Unreal's code generation changes the
rules around incomplete types vs. a hand-written class.

<!-- Append SB-M2, SB-M3, … sections below as milestones are completed. -->

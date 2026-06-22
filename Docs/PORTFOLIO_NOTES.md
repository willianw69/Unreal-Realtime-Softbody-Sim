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

<!-- Per-milestone sections (SB-M1 …) go here as they are completed, like the cloth project. -->

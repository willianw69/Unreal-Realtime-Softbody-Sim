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

## SB-M2 — Volume constraints → jelly ✅ 2026-06-22
**Shipped:** The defining soft body feature — GPU **per-tetrahedron volume preservation**. A box now
squashes on impact and bulges back to its rest volume instead of collapsing, matching the look of
commercial tools (Unity Obi Softbody / Zibra). Implemented as a *second* graph-colored constraint set
layered onto SB-M1's distance solver with no new topology — proof that the colored-Gauss-Seidel
framework generalizes across constraint types.

**Talking points proven in M2:**
- **PBD volume constraint on the GPU, from the math up:** signed tet volume `V=(1/6)dot(e1,e2×e3)`,
  constraint `C=V−V0`, analytic per-vertex gradients (cross products of opposing edges), mass-weighted
  scaling `s=−C/Σw|g|²`. One compute thread per tet, all 4 vertices corrected in place.
- **Graph coloring generalized to 4-vertex constraints:** tets are greedily colored so no two tets in a
  color share *any* vertex — the same race-free, in-place, per-color dispatch idea as the distance edges,
  but proving the scheme isn't specific to 2-endpoint constraints. Two independent constraint sets
  (edges + tets), each with its own colors, interleaved per solver iteration so they co-converge.
- **One tunable that tells the whole story:** `VolumeStiffness` 0→1 sweeps from distance-only (flattens)
  to volume-preserving (jelly) — a clean A/B demo of exactly what the volume term contributes.
- **Architecture payoff:** because SB-M1 already built the tetrahedra, M2 was "add a shader + a buffer +
  a coloring pass" with zero changes to the lattice, surface, readback, or render path. Compiled and
  worked first try.

**Resume bullet candidate:** "Implemented GPU volume-preserving soft bodies via per-tetrahedron Position
Based Dynamics volume constraints, graph-colored for race-free parallel solving, layered onto a shared
colored-Gauss-Seidel framework also driving a cloth simulator."

## SB-M3 — Mouse dragging ✅ 2026-06-23
**Shipped:** Real-time interactive dragging of the GPU-simulated body — click any surface point and pull
it around; the jelly stretches, follows, and springs back with momentum. The interaction that makes the
volume preservation *tangible* in a demo.

**Talking points proven in M3:**
- **Screen-space picking against GPU-driven geometry:** deproject the cursor to a world ray and pick the
  nearest *surface* particle from the same non-stalling position readback that feeds rendering — no extra
  GPU→CPU traffic, no separate pick buffer. A clean example of reusing existing data flow.
- **A grab as just another constraint:** the grabbed particle is pulled toward the cursor by a one-thread
  GPU pass placed after the constraint solve and before collision, so it's the solver's last word but
  still can't be dragged through the floor. The body follows because the distance/volume constraints
  propagate the displacement over substeps — emergent stretchiness, not hand-animated.
- **Momentum for free from PBD:** because velocity is re-derived from the position delta in Finalize, a
  released grab carries the velocity the drag imparted — fling-and-recoil with no extra bookkeeping.
- **Game/render thread split holds up under interaction:** input + picking on the game thread, the grab
  target pushed through the existing per-frame param snapshot, all sim math still on the render thread.

**Engineering note:** linker surfaced that `EKeys` lives in `InputCore` — the plugin needed that module
dependency even though the host game module already had it (plugin and host link independently).

## SB-M4 — Collisions (sphere/capsule + self) ✅ 2026-06-23
**Shipped:** The jelly now collides with authored sphere/capsule shapes (drapes and squashes over them)
and with itself (compressed regions don't interpenetrate). Completes the Obi/Zibra-style core feature set.

**Talking points proven in M4:**
- **GPU spatial-hash self-collision:** a Teschner-style hash grid built with atomics (`InterlockedAdd`)
  for the broadphase, then a race-free Jacobi gather over the 27 neighbour cells for the response — the
  one place atomics are used, deliberately confined to the build. Ported from the cloth simulator with a
  single topology change (2D grid 1-ring → 3D lattice 1-ring exclusion), reusing the prime-sized table and
  `MAX_PER_CELL` bucketing.
- **Analytic colliders for free from the shared shader:** sphere and capsule are one routine (a sphere is
  a degenerate capsule, A==B), so authored colliders lit up with zero shader changes — just Details-panel
  UI + a per-frame world-space transform. Shows the value of designing the collision primitive generically.
- **Buffer/dependency management under RDG:** self-collision needs a clean read snapshot, so the solve's
  in-place buffer was split into an A/B ping-pong with a `Solved` ref threaded through the substep
  (self-collision → grab → collision → finalize), letting RDG schedule correct read/write barriers.
- **Ordering as design:** self-collision runs before the mouse grab and the ground/shape collision so a
  hard pin or a solid surface always gets the final say on a particle's position that substep.

**End-to-end story:** with M1–M4 the project delivers every stated requirement — fully GPU-simulated, no
Chaos, runtime-generated constraints, volume-preserving jelly, mouse-draggable — plus ground/shape/self
collision, all on a from-scratch compute + RDG framework shared with a sibling cloth simulator.

## SB-M5/M6/M7 — Custom mesh, weight paint, XPBD ✅ 2026-06-23
**Shipped:** Any Static Mesh can be the soft body (a deformable bunny, not just a box); artists paint
vertex colors to make regions floppier or firmer; and the solver was upgraded to XPBD so that painted
softness behaves predictably regardless of solver settings.

**Talking points:**
- **Cage-based free-form deformation (SB-M5):** simulate a coarse tetrahedral *cage* auto-fit to the mesh's
  bounds, and skin the high-res render mesh to it with **barycentric embedding** — the same technique behind
  lattice deformers and tools like Obi. The entire existing GPU sim (constraints, volume, collisions, grab)
  was reused untouched; only where render vertices come from changed. Clean separation of "simulation proxy"
  vs "render geometry."
- **Art-directable material (SB-M6):** painted vertex colors → per-cage-particle weight → per-constraint
  stiffness. A clean pipeline from a DCC/editor paint channel through to the GPU solver, with a debug
  visualizer to confirm the transfer.
- **XPBD compliance (SB-M7) — and knowing *why* you need it:** the first weight-paint attempt used PBD
  stiffness scaling and the contrast washed out. Diagnosed it as PBD's well-known iteration-coupling (a low
  per-iteration stiffness still converges over many iterations), then implemented **XPBD** — per-constraint
  compliance with an accumulated Lagrange multiplier reset each substep (`α̃ = compliance/dt²`) — which makes
  stiffness a true material property independent of iteration/substep count. A strong "I understand the
  solver, not just the API" signal: recognizing a perceptual bug as a fundamental method limitation and
  fixing it at the math level.
- **Non-destructive upgrade:** XPBD added behind a toggle, with compliance 0 reproducing the prior stiff PBD
  behaviour, so earlier milestones didn't regress and the two methods can be compared side by side.

**Engineering details worth mentioning:** reading static-mesh LOD0 (positions/indices/UVs/colors) on the
CPU; barycentric point-in-tet tests with a cell-local search; an RDG typed-float buffer cleared via a uint
UAV view (0.0f ≡ 0u) to dodge an engine-version-specific clear overload; graph coloring making the λ
accumulator race-free for free.

**Resume bullet candidate:** "Extended a GPU soft-body solver with cage-based free-form deformation of
arbitrary meshes, vertex-color-painted per-region stiffness, and an XPBD compliance solver for
iteration-count-independent material stiffness."

## SB-M8 — Distance-field collision vs arbitrary meshes ✅ 2026-06-23
**Shipped:** The soft body collides against any mesh in the scene (ramps, statues, the environment) with no
authored collider shapes, by sampling Unreal's Global Distance Field.

**Talking points:**
- **GDF collision from a standalone compute pass:** a `FSceneViewExtension` snapshots the renderer's Global
  Distance Field parameters after the base pass into a render-thread cache, which a custom RDG compute pass
  then samples (`GetDistanceToNearestSurfaceGlobal` + gradient) to project particles out of the nearest
  surface. Demonstrates integrating a custom simulation with the deferred renderer's own data (the same
  field Niagara GPU collision uses), and understanding *why* the GDF — not per-mesh distance fields — is the
  representation Unreal exposes to a standalone shader.
- **Bug hunt — disappearing mesh:** diagnosed a "mesh vanishes when it moves far" report as stale scene-proxy
  bounds (computed once at spawn) frustum-culling the body once it left its original box; fixed by
  recomputing bounds from the deformed verts each frame and pushing them via `UpdateBounds` +
  `MarkRenderTransformDirty`. A clean example of reasoning from a visual symptom to a render-thread cause.
- **Layered collision:** ground plane + analytic sphere/capsule + spatial-hash self-collision + scene GDF
  all compose in one substep, each as its own race-free pass.

**Resume bullet candidate:** "Added distance-field collision letting a GPU soft body collide with arbitrary
scene geometry by sampling Unreal's Global Distance Field from a custom compute pass via a scene view
extension."

## SB-M9 — Multi-body collision ✅ 2026-06-24
**Shipped:** Multiple soft body actors collide with and pile on each other (jelly-on-jelly), coordinated by
a world subsystem running one shared GPU pass per frame.

**Talking points:**
- **Coordinating independent simulations:** each body sims in its own buffers/dispatch; a `UWorldSubsystem`
  registers them and runs a single cross-body collision pass, designed as a *post-sim positional projection*
  so it composes cleanly without restructuring any body's substep. A clean example of adding global behaviour
  over per-object systems with minimal coupling.
- **Reuse, again:** the cross-body pass is the self-collision spatial hash with one rule changed — repel
  particles whose body-id differs instead of whose lattice cells are adjacent. Same race-free Jacobi gather,
  same prime-sized hash grid. The framework's third reuse (cloth → soft body → self → inter-body).
- **GPU buffer plumbing:** gather N bodies' separate pooled position buffers into a combined buffer via
  RDG buffer-region copies, build/repel over the union with a per-particle body-id, then scatter corrections
  back — all on the GPU, transient per frame, with render-thread-safe resource capture.

**Resume bullet candidate:** "Added inter-body collision for multiple GPU soft bodies via a world subsystem
that runs a shared spatial-hash repulsion pass over all bodies' particles each frame."

## SB-M10 — Interactive cutting + visual split ✅ 2026-06-25
**Shipped:** Slice a soft body with a right-mouse swipe — it splits into separate jiggling chunks and the cut
surface opens up live. The most involved milestone: a topology change at runtime.

**Talking points:**
- **Runtime topology change on a GPU sim:** the body holds together purely through its constraints, so
  "cutting" = flagging the constraints crossing a plane as broken (the solve shaders skip them) and the pieces
  fall apart naturally — no re-simulation, no re-init. A clean demonstration of *why* a constraint-based
  formulation makes fracture cheap.
- **Live surface re-extraction:** the rendered surface is rebuilt from the surviving tetrahedra each cut
  (a face used by exactly one un-cut tet is boundary), so interior faces exposed by removed tets become the
  visible cut surface — pushed to the GPU through a dynamic index buffer. This is the bulk of the work and the
  part that makes the cut *look* real.
- **Reuse end-to-end:** the solve passes only gained a skip; the resulting chunks interact through the existing
  self-collision; the cut plane is tested CPU-side from the same position readback that drives rendering and
  mouse picking. Only the scene proxy gained a new capability (mutable index buffer).
- **Honest scope line:** clean visual cutting targets the lattice/box bodies; cutting an embedded custom mesh
  needs render-mesh triangle splitting (queued as SB-M11) — a good example of scoping an ambitious feature to a
  shippable slice and naming the follow-up.

**Resume bullet candidate:** "Implemented interactive cutting of a GPU soft body — severing constraints along a
swipe plane and re-extracting the boundary surface from the surviving tetrahedra with a dynamic index buffer so
the body splits into separate pieces in real time."

## Project status
M1–M10 complete: a from-scratch GPU tetrahedral-XPBD soft body in UE 5.7 (no Chaos) with volume preservation,
mouse dragging, ground/sphere/capsule/self/distance-field/multi-body collision, custom-mesh embedding,
vertex-color weight-painted stiffness, and **interactive cutting/splitting** — all on a custom compute + RDG
framework shared with a sibling cloth simulator. Next: SB-M11 (cuttable custom meshes), then SB-M+ polish.

<!-- Append SB-M11 / further sections below as work is completed. -->

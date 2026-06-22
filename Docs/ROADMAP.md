# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-22.

| Milestone | Title | Status |
|---|---|---|
| M0 | Project bootstrap (C++ host builds) | ✅ |
| SB-M1 | GPU lattice solid (framework port + distance constraints) | ⏳ |
| SB-M2 | Volume constraints → jelly (tetrahedra) | ⏳ |
| SB-M3 | Mouse dragging (pick + grab constraint) | ⏳ |
| SB-M4 | Collisions (ground / sphere / self) | ⏳ |
| SB-M+ | XPBD compliance, Sphere/SDF shapes, mesh embedding, profiling | ⏳ (stretch) |

## Notes per Milestone

### M0 — Project bootstrap ✅
`SoftBodyDemo` UE 5.7 C++ host project created (uproject, Target.cs ×2, Build.cs, module, Config,
.gitignore/.gitattributes) mirroring `ClothSimDemo`. Builds clean via CLI. No plugin yet.

### SB-M1 — GPU lattice solid ⏳ (do this first)
Create `Plugins/SoftBodySim` by porting the ClothSim GPU framework. Runtime-generate a 3D particle
**lattice** (box) and **distance constraints**, graph-colored. Pipeline: `SBPredict` → colored
Gauss-Seidel distance solve → `SBFinalize` → readback. Render the box boundary as a lit mesh via a
ported `FSoftBodyMeshProxy`. Add a built-in **ground plane** and an **anchor** option (pin one face)
for the first test. **Result:** a springy deformable box that falls, jiggles, and rests on the floor
(volume not yet preserved — expected). Proves the full 3D GPU pipeline.

### SB-M2 — Volume constraints → jelly ⏳
Generate **tetrahedra** (6-tet Kuhn split per cell) + per-tet **volume constraints**
(`SBSolveVolume.usf`), graph-colored as a second constraint set. Volume preserved → jelly. The
headline feature. Tune distance-vs-volume relative stiffness + iterations.

### SB-M3 — Mouse dragging ⏳
Deproject cursor → ray; pick nearest boundary particle (CPU from readback); push grab target; grab
step pulls it (soft attachment). Interactive poke/pull/stretch.

### SB-M4 — Collisions ⏳
Port the cloth collision passes: built-in ground plane (already in SB-M1), sphere/capsule colliders,
and spatial-hash **self-collision**, so the jelly squashes on the floor and against shapes.

### SB-M+ — Stretch ⏳
XPBD compliance per constraint (proper stiffness independent of iteration count); Sphere/arbitrary
shapes via SDF voxelization + boundary-from-tets extraction; embed a smooth render mesh skinned to
the tets; multiple bodies; zero-copy GPU vertex write; `stat GPU`/Insights profiling pass.

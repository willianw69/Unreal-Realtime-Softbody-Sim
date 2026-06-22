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

<!-- Append SB-M1, SB-M2, … entries below after each milestone is implemented + verified in-editor. -->

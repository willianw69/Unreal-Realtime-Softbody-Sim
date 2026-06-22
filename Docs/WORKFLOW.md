# WORKFLOW.md

> The development pipeline for RT_SoftBody — identical in spirit to the RT_ClothSim project.
> Docs are a deliverable: any future session/engineer must continue with zero prior context.

## Golden rule — verify before you document or commit
A clean CLI build is **necessary but not sufficient**: shaders compile at runtime, and behaviour
must be eyeballed in-editor. **Do NOT update docs or commit until the USER confirms the milestone
works in the editor.** After a green build, summarize what to verify and **wait** for the go-ahead.

## Per-milestone loop
1. **Implement** the milestone's code (see `ROADMAP.md` / `HANDOFF.md` for scope).
2. **Build** via the CLI (editor closed — see below). Fix errors until it compiles + links.
3. **Hand off for verification:** tell the user exactly what to test in-editor; **wait**.
4. **Only after the user confirms it works:**
   - Update docs: `PROJECT_STATE` (overview/current/completed/next/decisions/limitations/future),
     `ROADMAP` (✅🔄⏳), `DEVLOG` (APPEND date/what/why/problems/solutions/perf/next — never overwrite),
     `HANDOFF` (current state + recommended prompt), `PORTFOLIO_NOTES`, `ARCHITECTURE` (if it changed).
   - Commit with a conventional message: `feat(SB-M1): ...`, `fix(render): ...`, `docs: ...`.
   - Push (once a git remote is set up — see below).

## How to build (CLI)
1. **Close the SoftBodyDemo editor first.** Live Coding from *any* open UE editor globally locks
   CLI/VS builds (`Unable to build while Live Coding is active`). Header/`UPROPERTY`/new-shader
   changes need a full rebuild + editor restart anyway (Live Coding can't hot-patch them).
2. Run:
   ```
   "E:/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" SoftBodyDemoEditor Win64 Development -Project="E:/ClaudeCode/RT_SoftBody/SoftBodyDemo.uproject" -WaitMutex
   ```
   Expect `Result: Succeeded`.
3. **Run:** open `SoftBodyDemo.uproject`, drop a SoftBody actor into the level, Play. Assign a
   **Two-Sided** material to see the surface shaded correctly.
   - `.cpp`-only changes can alternatively be hot-patched in the open editor with **Ctrl+Alt+F11**
     (Live Coding), then restart PIE — but header/`UPROPERTY`/new-`.usf` changes require the full
     close → CLI build → reopen loop.

## Engine / environment
- Engine: `E:\Epic Games\UE_5.7`. Toolchain: VS 2022. Platform: Win64, Development.
- `r.ShaderDevelopmentMode=1` is set so plugin compute shaders report errors and can iterate.
- Sibling project to copy framework code from: `E:\ClaudeCode\RT_ClothSim` (plugin `ClothSim`).

## Git
- **Remote:** `https://github.com/willianw69/Unreal-Realtime-Softbody-Sim.git` (GitHub, branch `main`).
- Initialize the local repo if not already done (`.gitignore`/`.gitattributes` are in place), add the
  remote above, and make an initial commit of the M0 bootstrap + `Docs/`.
- Commit **per milestone, after in-editor verification** (the gate above), then push `main`. Use
  conventional messages: `feat(SB-M1): ...`, `fix(...)`, `docs: ...`.
- End commit messages with the project's standard co-author trailer if used.

## Inherited gotchas (do these from the start — learned the hard way in cloth)
- **Normals:** compute smooth normals with `Cross(E2,E1)` so they match UE's left-handed winding →
  two-sided materials light both faces (cloth M9).
- **No RHI breadcrumb scopes** (`RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE`) on the plugin's standalone
  `FRDGBuilder` — it crashes at `Execute()` (cloth M8). Use per-pass `RDG_EVENT_NAME` for profiling.
- Shader virtual paths include the subfolder: `/SoftBodySim/Private/SBPredict.usf` (map `/SoftBodySim`
  → `Shaders/` at module startup).
- `TUniquePtr<FRHIGPUBufferReadback>` needs the complete type for its dtor → declare the resources
  struct's ctor/dtor out-of-line in the .cpp that includes `RHIGPUReadback.h` (cloth C4150 fix).

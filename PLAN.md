# Big Powder: plan

Status: plan only, nothing built yet. "Big Powder" is a working name and easy to change.

## Goal

A native Linux app with The Powder Toy's elements and mechanics (v100.1.400 as the baseline), on a much
bigger zoomable world, simulated on the GPU. It should run at 30 to 240 fps at up to 4K and load TPT saves.
Licence: GPL-3.0, because we port TPT code. Ports to other platforms may come later.

## The Powder Toy today (facts the design rests on)

- Latest release: v100.1.400 (8 Aug 2026). Snapshot 401 is a pre-release (4 Oct 2026).
- Written in C++ with SDL, built with Meson. Lua/LuaJIT scripting, FFTW for Newtonian gravity. GPL-3.0.
- The sim is 612×384 px. `CELL = 4` gives a 153×96 grid for air, heat, gravity and walls. These are
  compile-time constants in `src/simulation/SimulationConfig.h` (its comment: larger CELL "to prevent
  extreme lag"). `NPART = XRES*YRES` = 235,008 particles.
- About 195 element source files are in `src/simulation/elements`.
- Particle fields: `type life ctype x y vx vy temp tmp tmp2 tmp3 tmp4 flags dcolour`. Positions and
  velocities are floats.
- Particles live in an array. `pmap` (one particle per pixel, for lookups) and `photons` are index maps.
  Several particles can share a pixel ("stacking"). Saves store a count per pixel, so stacking is part
  of the save format.
- The sim takes one step per frame, updating particles one by one in array order on one CPU thread, so
  sim speed is tied to frame rate (the default cap is 60).
- Saves: OPS1 is a 12-byte header followed by bzip2-compressed BSON (`parts`, `partsPos`, `wallMap`,
  `pressMap`, `vxMap`/`vyMap`, `ambientMap`, `blockAir`, `gravity`, `signs`, `palette`, ...). The old
  PSv format is also readable. The save size is stored in cells as single bytes and checked against
  153×96, so an OPS save can't hold a world bigger than TPT's.

## Why it can't simply be made bigger

Every step visits every particle serially, and each element's update code reads and changes its
neighbours. Cost grows with area: a 4K world is 35× TPT's, so a CPU at TPT's speed would manage under
2 fps when full. Even 24 threads don't close that gap at 60+ steps per second.

## Decisions so far

| Question | Answer |
|---|---|
| Form | Native Linux app first, ports later |
| World | Zoomable; possibly several game modes |
| TPT saves | Load them where possible |
| Licence | GPL port of TPT code is fine |
| Location | `~/dev/big-powder` |

## Game modes

1. **Classic**: 612×384, running TPT's own simulation code on the CPU. It behaves exactly like TPT,
   every save works, and the full Lua API is there. It's also the reference for testing the GPU engine.
2. **Screen**: 3840×2160 on the GPU engine. At full zoom-out, one cell is one pixel on a 4K monitor.
3. **Huge world**: bigger than the screen (start at 7680×4320, about 33M pixels), using the GPU engine
   with sleeping chunks. You can zoom out below 1:1.
4. Later, if wanted: scenario or challenge modes.

## World, zoom and camera

- For Screen mode, one cell per screen pixel at full zoom-out is the right baseline. 8.3M cells is 35×
  TPT.
- Zoom in as far as you like. Rendering only touches visible cells, so cost falls as you zoom in. Past
  about 8×, cells show as squares; smoothing can be an option.
- Air, heat and gravity sit on a 4×4-px grid, as in TPT. Their display gets bilinear filtering so they
  don't look blocky when zoomed in.
- Zooming out past 1:1 (Huge mode) blends several cells into each screen pixel, using a pyramid of
  reduced images.
- Brush and tool sizes are measured in cells and drawn in screen space.
- TPT's zoom magnifier is kept as a tool.

## Frame rate and sim speed

TPT takes one sim step per frame, so 240 fps would run the physics 4× faster. Here they're separate:

- The sim runs at a fixed tick rate, 60 ticks/s by default so it feels like TPT. A speed setting runs
  from 0.25× to 4×, with pause and single-step.
- Rendering, camera and UI run at display refresh, 30 to 240 fps.
- If the sim can't keep up, it slows down (as TPT does) instead of skipping ticks. The HUD shows both
  fps and tps.

## Architecture

### Stack

- C++20, CMake and Ninja.
- SDL3 for the window and input (native Wayland); Vulkan 1.3 for compute and drawing.
- Shaders in **Slang**, compiled to SPIR-V. I chose it over GLSL because ported element code can stay
  close to C++ (structs with methods, generics, modules), and Slang can later target Metal, D3D and CUDA
  for ports. GLSL (`glslc`) is the fallback if Slang causes trouble.
- Rejected:
  - CUDA: the quickest port, but NVIDIA-only, which blocks ports.
  - wgpu/WGSL: limits on atomics and buffer bindings, and C++ ports into WGSL are slower to write.
  - CPU multithreading: not enough for 4K at 60+ ticks/s.
- Libraries:
  - Dear ImGui for the first UI.
  - VkFFT for Newtonian gravity on the GPU.
  - TPT's GameSave and BSON code with bzip2 for OPS saves.
  - zstd for our own world format.
  - TPT's Lua bindings for scripting.
- TPT's source is vendored as a git subtree, pinned to `v100.1.400`. Later tags are merged in when
  they're wanted.

### Data model on the GPU (a mirror of TPT's)

Keeping TPT's layout means element code ports almost line by line and saves load exactly:

- A particle array (structure-of-arrays with TPT's fields). Capacity is the world's pixel count plus
  room for stacking.
- `pmap` and `photons` maps (uint32 per pixel).
- Maps at 4×4 px: pressure, vx, vy, ambient heat, walls, fan velocity, blockAir, gravity mass and force.
- An element table (properties, transitions, the `can_move` matrix) built from TPT's element
  definitions at startup.
- Global state: wifi channels, portals, stickmen, Game of Life rules, element counts, RNG state.

Memory: 4K is about 8.3M × 56 B ≈ 470 MB of particles, under 1 GB with all maps. Huge mode (33M) is about
2 GB. That fits in 16 GB, but it's checked against what the desktop and ollama are already using.

### One sim tick

This follows TPT's own order (BeforeSim, UpdateParticles, AfterSim):

1. Air (pressure and velocity) and ambient heat: stencil passes on the 4×4 grid.
2. Newtonian gravity: FFT convolution, only when it's enabled.
3. Particle updates: element update functions plus movement. This is the hard part (see below).
4. Bookkeeping: sparks and electronics state, the wifi channel swap, Game of Life, element counts,
   compaction, chunk activity.
5. Render.

### The hard part: updating particles in parallel

TPT updates one particle at a time. Each update can read, change, create or delete its neighbours (mostly
within 1 to 2 px). Particles move with float velocity along a line and can swap with lighter ones. Doing
all of this at once on a GPU has two problems: two particles can claim the same pixel, and two updates can
change the same neighbour.

**Decided in Phase 0** (details and numbers in [docs/phase0-results.md](docs/phase0-results.md)). The
original idea of 16-px tiles didn't survive contact with TPT's code: liquids scan up to 30 px sideways every
tick, so no small tile size keeps moves apart. What was built instead:

- **Pixel colouring.** Live particles are sorted into N×N colour lists by position, and each tick runs N²
  passes. Particles in one pass are a multiple of N px apart in both axes. TPT's reactions reach at most
  2 px, so N=3 keeps a particle's update from racing its neighbours' updates, and N=5 makes the passes
  strictly race-free. N=5 is the default (accuracy first); N=3 is a faster option. The grid offset and pass order are randomised each tick.
- **Atomic moves.** Movement keeps TPT's logic (`PlanMove`, `do_move`, `MovementPhase`, the liquid scan).
  A mover locks its own cell and compare-and-swaps into the target; losing a race fails like a blocked move.
  A mover never displaces a particle whose own update is running in the same pass.
- **`pmap` rebuilt each tick**, like TPT's `RecalcFreeParticles`.
- **Still planned:**
  - Long-reach elements get their own passes, or write "intents" that are resolved after the main pass:
    photons, LIGH, BRAY/ARAY rays, DTEC/TSNS/LSNS sensors, CONV, PIPE chains, wifi, portals, VOID/BHOL and
    stickmen.
  - The `pmap` layout changes from TPT's: an entry holds `id + 1` (31 bits) and a lock bit, and types live
    in their own array. TPT packs the type into `pmap`, which would limit ids to about 8M, too few for
    worlds above 4K.

### Sleeping chunks

- Chunks are 64×64 px. A chunk is marked active when something changes in it or next to it, and inactive
  chunks are skipped (sand piles at rest, solid walls). Air and heat still run everywhere, but on the
  grid that is 4× coarser.
- Design target: a fully active 4K world at 60 ticks/s, and 240 ticks/s when much of the world is
  asleep. Rough bandwidth sums say the first fits on the RTX 5080 and a fully active world at 240 ticks/s
  probably doesn't. Phase 0 measures it.

### Rendering

- A compute pass colours each visible cell, using TPT's graphics behaviour: flat, blur, glow, fire add,
  spark flash and deco. A fire and glow buffer fades each frame, as in TPT.
- TPT's display modes: velocity, pressure, persistent, fire, blob, heat, fancy, nothing, heat gradient,
  life gradient, destructibility and vorticity.
- The sim and the renderer share GPU buffers. Only HUD numbers and the cell under the mouse go back to
  the CPU.

### Saves

- OPS (`.cps`, `.stm`) and old PSv saves load through TPT's own GameSave code. They can be placed
  anywhere in the big world as a stamp, or opened in Classic mode.
- TPT's local saves and stamps folders can be imported.
- Areas up to 612×384 can be saved as OPS, to share back to TPT.
- Big worlds use our own format: chunked, zstd-compressed, with the same particle fields.
- Online save browser (powdertoy.co.uk): later, and only with your OK, since it talks to their servers.

### Scripting

- Classic mode has TPT's full Lua API.
- GPU modes get Lua for tools, UI and automation, with batched reads and writes (no per-particle round
  trips to the GPU). Lua custom elements with their own update functions can't run on the GPU. Custom
  elements defined by data alone (properties, transitions) can.

### Testing against TPT

- The Classic engine is the reference. A test harness loads a save into both engines, runs N ticks and
  compares element counts, mass, temperature histograms and pressure statistics. Update order differs, so
  an exact match isn't expected; each test has its own tolerances.
- Each element category gets a set of test saves.
- Benchmarks: 4K full of water, 4K full of sand, a mixed reactive scene and a large electronics build.
  Each reports the time spent in every pass.

## Phases

0. **Setup and engine spike.** *Done 2026-10-20; see [docs/phase0-results.md](docs/phase0-results.md).*
   - Install the toolchain.
   - Open an SDL3 window with Vulkan, and draw a 4K buffer with pan and zoom at 240 fps.
   - Build a tile-scheduler spike with about 6 ported elements (DUST, WATR, OIL, STNE, FIRE, METL), and
     measure ticks/s with all of 4K active for variants A and B.
   - **Gate:** I report the numbers to you and we decide on the scheduler.
   - **Results:**
     - 5×5 pixel colouring with atomic moves (3×3 is the faster option).
     - At 5×5, a full 4K tank of water (8.25M particles) runs at about 120 ticks/s; typical scenes at 150–400.
     - No particles lost or duplicated.
     - The viewer reaches about 2,200 fps uncapped.
1. **Classic mode.** Vendor TPT at v100.1.400, build its simulation without its UI, and drive it with
   our renderer and input. Saves load. The result is playable and is the test reference.
2. **GPU core.** The data model; movement following TPT's `do_move`/`try_move`/`eval_move`; gravity
   modes; edge modes; air; ambient heat; heat conduction; state changes; walls; brush tools.
3. **Elements in batches**, each checked against Classic:
   1. Powders, liquids, gases, solids.
   2. Fire and explosives.
   3. Life: PLNT, SEED, VINE, WOOD, SPNG and the rest.
   4. Electronics: SPRK, conductors, semiconductors, switches, sensors.
   5. Nuclear and light: NEUT, PHOT, FILT, PLUT, URAN and the rest.
   6. Special: PIPE, CLNE, wifi, portals, STKM/FIGH, Game of Life, LIGH, rays, CONV, BHOL/WHOL.
4. **Newtonian gravity, deco, every display mode, signs.**
5. **Huge world mode.** Tune chunk sleeping, add zoom-out below 1:1 and our own save format.
6. **TPT-style UI.** Element menus, tools, TPT's keyboard shortcuts, undo, settings.
7. **Extras.** Lua in the GPU modes, online saves (if you OK it), packaging (.deb or AppImage), then
   ports.

## Risks

- **Scheduler.** Speed against faithfulness is the main unknown, which is why Phase 0 has a gate.
- **Timing-sensitive builds.** Electronics and filter contraptions may act differently in GPU modes.
  Classic mode is the fallback that always behaves like TPT.
- **Stacked particles.** The data model supports them, but TPT's handling of stacks is quirky, and
  matching it in parallel passes will take care.
- **GPU hangs.** A long compute dispatch can trip the driver watchdog (an Xid 8 hang happened on this PC
  on 2026-10-11). Dispatches stay short; `nvidia-smi` is checked before runs; tests start small.
- **Scope.** About 195 elements is a lot. Batches keep each milestone playable.

## Toolchain to install in Phase 0 (each install needs your OK)

- apt packages:
  - Build: `cmake ninja-build`
  - Window and Vulkan: `libsdl3-dev vulkan-tools vulkan-validationlayers glslc`
  - Libraries: `libzstd-dev libfftw3-dev libpng-dev libjsoncpp-dev liblua5.4-dev`
  - Only for online saves: `libcurl4-openssl-dev`
- The Slang compiler: a prebuilt release from github.com/shader-slang/slang, placed in `tools/`.
- Already installed: g++, git, libvulkan-dev, libbz2-dev, the NVIDIA 595 driver with Vulkan.

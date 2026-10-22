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
  strictly race-free. Since 2026-10-21 the default is a 7×7 grid (49 passes), which measured closest to TPT ([docs/accuracy.md](docs/accuracy.md)) and is race-free for reactions reaching 3 px. The grid offset and pass order are randomised each tick.
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
- Areas up to 612×384 can be saved as OPS, to share back to TPT. *Done 2026-10-23:* written by TPT's own
  `GameSave` as `Simulation::Save` would (ctrl+shift+S); TPT loads them back exactly as it loads its own saves.
- Big worlds use our own format: chunked, zstd-compressed, with the same particle fields. *Done 2026-10-23:* `.bpw`
  world files keep everything TPT's saves keep plus the engine's own state, so a loaded world continues exactly.
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
     - Pixel colouring with atomic moves; now 7×7 (see Phase 1 notes).
     - At 5×5, a full 4K tank of water (8.25M particles) runs at about 120 ticks/s; typical scenes at 150–400.
     - No particles lost or duplicated.
     - The viewer reaches about 2,200 fps uncapped.
1. **Classic mode.** *Done.* Vendor TPT at v100.1.400, build its simulation without its UI, and drive it with
   our renderer and input. Saves load. The result is playable and is the test reference.
   - *Progress (2026-10-21):*
     - TPT is vendored as a subtree and built by its own Meson build. `ClassicSim` wraps it: step, saves,
       brush, TPT's renderer.
     - `--compare` checks the GPU engine against it ([docs/accuracy.md](docs/accuracy.md)). That found the
       missing air solver, which was pulled forward from Phase 2 and ported.
     - The colour grid changed to 7×7 based on the measurements.
     - *Done 2026-10-21:*
       - Classic mode is in the viewer (`--mode classic`, `M` to switch).
       - TPT saves load: replacing the world in Classic, as stamps in GPU worlds. Walls and air in saves
         wait for the GPU engine to support them.
       - Classic saves `.cps` files.
       - `--export-tpt` writes test scenes as TPT saves.
2. **GPU core.** *Done 2026-10-22 (Newtonian gravity is in Phase 4):*
   - *Done:*
     - all 19 TPT wall types: blocking, filters, absorb, E-wall/E-hole, detectors with TPT's `set_emap` flood
       fill, fans, stasis, air blocking;
     - edge modes void/solid/loop;
     - gravity modes vertical/off/radial/custom;
     - ambient heat (`update_airh`) and particle-to-air heat exchange;
     - a settings buffer;
     - viewer keys for the settings and a wall brush;
     - `CheckStacking` (piles of more than 5 particles in a cell collapse into NBHL), counted only in cells where
       particles stack;
     - pasting saves as TPT's `Simulation::Load` does: what was in the way is removed, stacked particles are kept,
       SOAP's links are renumbered, and the save's air comes with it.
   - Measured against TPT in [docs/accuracy.md](docs/accuracy.md).
   - The original scope: the data model; movement following TPT's `do_move`/`try_move`/`eval_move`; gravity
   modes; edge modes; air; ambient heat; heat conduction; state changes; walls; brush tools.
3. **Elements in batches**, each checked against Classic. *Done 2026-10-22: all 194 of TPT's elements.*
   - Element properties and defaults come from TPT's own tables.
   - Enabled on the GPU:
     - 34 property-only elements;
     - water and fire;
     - the water family: DSTW, SLTW, WTRV, ICEI, SNOW, RIME, FOG, with TPT's special temperature
       transitions;
     - plants and their partners: SEED, PLNT (tree growth with genes), VINE, WOOD, SPNG, GOO, O2, CO2, COAL,
       BCOL. This brought TPT's `create_part` into the update passes, with ids from a per-tick free list;
     - lava and melting: LAVA (solidifying back into what melted, rock formation under pressure), THRM, CNCT,
       GLAS, BMTL, BRMT, IRON (rust), GOLD, TTAN, CLST, CRMC. Every meltable element on the GPU now melts;
     - chemistry and gases: ACID, CAUS, CBNW, BASE, H2, BOYL, RFRG, RFGL, PLSM, NBLE (H2's and NBLE's fusion
       waits for NEUT and PHOT);
     - more liquids and powders: GEL (with its heat and flow special cases), MERC, FRZW, FRZZ, YEST, ANAR,
       QRTZ, PQRT, SLCN, GLOW, RSST, RSSS, VIRS, VRSS, VRSG;
     - electronics: SPRK with TPT's conduction rules, PSCN, NSCN, INWR, INST (FloodINST), NTCT, PTCT, BTRY,
       SWCH, WIRE, ETRD (arcs to the nearest free electrode), TESC and LIGH (lightning); sparks from
       electrified walls and from the brush; GOLD and SLCN take sparks;
     - sensors (DTEC, TSNS, PSNS, LSNS, VSNS, LDTC; their FILT modes wait for FILT) and explosives (FUSE, FSEP,
       IGNT, BANG, BOMB, EMBR, C5, DEST, FWRK, FIRW, THDR). Drawing onto DTEC/LDTC sets their ctype;
     - cloners (CLNE, BCLN, PCLN, PBCN), VOID, PVOD, BHOL, WHOL, NBHL, NWHL (their Newtonian gravity came with
       Phase 4), CONV, SHLD1-4, PUMP, HSWC, DLAY; eval_move's per-particle cases and try_move's voids and holes;
     - energy particles: TPT's photons map, their movement (reflection by boundary-traced normals, refraction
       in glass by wavelength, the photoelectric effect), PHOT, NEUT (with its push through penetrable
       material), ELEC, PROT (collisions make heavier elements), GRVT, PLUT, URAN, DEUT, ISOZ, ISZS, POLO, and
       the fusion of H2, NBLE and CO2;
     - exotic matter and light: AMTR, SING, WARP, EXOT, VIBR, BVBR, FILT (all 12 modes, and the sensors'
       FILT read-outs), BIZR/BIZRG/BIZRS, INVIS, LCRY, GPMP (its gravity came with Phase 4), TUNG;
     - force elements and more: ACEL, DCEL, RPEL, FRAY, GBMB (its gravity came with Phase 4), DMG, BREC, HEAC,
       STOR; try_move's CNCT and GBMB rules.
     - beams, wireless and EMP: ARAY and BRAY, CRAY, DRAY (beams are cast after the passes, one thread each,
       since they cross the whole world), WIFI's 101 channels, EMP's effect on electronics (each particle
       gathers the sparks around it). Drawing onto CRAY and DRAY sets their ctype.
     - portals and more: PRTI and PRTO (a shared store per channel, its slots claimed with atomic swaps), LITH,
       PTNM, MORT, and LOVE and LOLZ (each 9x9 block redrawn before the passes).
     - the Game of Life (LIFE, all 24 built-in rulesets): TPT's SimulateGoL as three passes before the update
       passes, gathering each cell's neighbours in id order so its neighbour lists come out exactly as TPT's.
     - SOAP: bubble film linked into chains by particle id (links to SOAP that is gone are dropped in the
       survivor's own update, which keeps TPT's detach out of every kill).
     - PIPE and PPIP: the BRCK border, the colour pattern, particles moved at most two pipes a tick (TPT's
       recursion unrolled), PPIP's spark triggers flooded over the network after the passes, HEAC making pipes
       conduct heat.
     - PSTN and FRME: pistons sparked in the colour passes, then moved after them one at a time in the order
       they were sparked, pushing or pulling the stack in front of the arm and any FRME frame with what rests
       on it.
     - TRON: heads steer by looking ahead along their tail length and leave fading tails, entirely in the colour
       passes (they only ever create their next head next to themselves).
     - stick men: STKM and STKM2 (steered from the keyboard), FIGH (up to 100 fighters, steering themselves),
       SPAWN and SPAWN2. Their state (legs, held element) is TPT's playerst in its own buffer. One thread before
       the colour passes takes stock of which exist (TPT does that in their create and kill hooks), respawns
       the players and runs their update in id order; the head then moves in its colour pass. Drawn as figures
       over the frame.
     - GRAV (marks itself while moving) and EQVE.
   - Left for later phases: element Graphics functions and display modes, Newtonian gravity (GRVT, GPMP, GBMB,
     NBHL/NWHL), custom Game of Life colours. *The first two came with Phase 4.*
   - Reactions into elements that aren't ported yet switch on by themselves once those are.
   - Element ids come from TPT's element list (`tools/gen-element-ids.py`) and are checked against the TPT
     build at startup.
   1. Powders, liquids, gases, solids.
   2. Fire and explosives.
   3. Life: PLNT, SEED, VINE, WOOD, SPNG and the rest. *Done.*
   4. Electronics: SPRK, conductors, semiconductors, switches, sensors.
   5. Nuclear and light: NEUT, PHOT, FILT, PLUT, URAN and the rest.
   6. Special: PIPE, CLNE, wifi, portals, STKM/FIGH, Game of Life, LIGH, rays, CONV, BHOL/WHOL.
4. **Newtonian gravity, deco, every display mode, signs.** *Done 2026-10-23 (deco tools moved to Phase 6):*
   - **TPT's renderer on the GPU.** TPT draws particle by particle in id order into a 612×384 image: an
     element's Graphics function (or the default one: liquids blur, gases only feed the fire buffer) gives a
     pixel mode and colours, then flat pixels overwrite, blends and saturating adds follow, glows, blurs, blobs
     and flares reach into neighbouring pixels, and FIRE_* modes feed a cell-sized fire buffer that is drawn as a
     soft glow and smoothed in place every frame. The port keeps the result exact by keeping TPT's order:
     - `gfx`: one thread per particle computes its graphics (all 76 Graphics functions, PROP_HOT_GLOW, the
       display preset's mode mask, heat/life/gradient/basic colour modes, deco). What a particle draws on its
       own pixel is kept with it; what it draws anywhere else (and what hidden stacked particles draw) is
       appended as fragments (id, step, operation, colour) to per-pixel lists, as order-independent
       transparency does; fire-buffer writes go to per-cell lists the same way.
     - `resolve`: per pixel, the base (air display, walls) and then the top particle's, the top energy
       particle's and the listed fragments' operations, sorted by (id, step) and applied with TPT's integer
       arithmetic. Per fire cell, likewise, then the fire glow and TPT's in-place smoothing (an anti-diagonal
       wavefront reproduces its order).
     - The world is drawn at one pixel per cell into a world-sized image that the viewer samples when zooming.
     - Checked pixel by pixel against TPT's own renderer on the same simulation state (`--render-check`).
     - *Done:* all 76 Graphics functions, every pixel mode, the fire buffer and its glow, walls (with blob
       display's blobs), the air displays (pressure, velocity, heat, vorticity), persistent display, the dynamic
       heat display's moving range, the EMP flash, stick men. All 13 presets match TPT pixel for pixel except
       where TPT itself draws at random (flicker of sparks and flares, GLOW, GOLD). A 4K world draws in 2 ms
       (Fire Display) to 8 ms (Fancy Display).
   - **Newtonian gravity.** *Done:* TPT's FFT convolution in compute shaders (`gravity.slang`), with TPT's
     two-tick delay and GRAV walls' mask; every mass source (NBHL, NWHL, GRVT, GPMP, GBMB) and every use of the
     field (movement, liquids spreading across it, high-gravity transitions, DEUT, O2's fusion, stick men); the
     field display, the gravity zones and the warp effect (lensing). Checked against TPT in
     [docs/accuracy.md](docs/accuracy.md).
   - **Streamlines and signs.** *Done:*
     - Stream walls trace the air's velocity and draw TPT's icon, in DrawWalls' order among the walls.
     - Signs show their text as TPT does: `{t}`, `{p}`, `{a}`, `{type}`, `{ctype}`, `{life}`, `{tmp}` and `{tmp2}`
       read back from the GPU, link signs' colours and BlendText's escape codes, in TPT's font. They come with
       loaded saves.
   - Deco tools (painting decoration colours with a brush) are brush tools, so they move to Phase 6 with the other
     tools. Decorations themselves are drawn already.
5. **Huge world mode.** Tune chunk sleeping, add zoom-out below 1:1 and our own save format.
6. **TPT-style UI.** Element menus, tools (including the deco tools), placing and editing signs, TPT's keyboard
   shortcuts, undo, settings.
7. **Extras.** Lua in the GPU modes, online saves (if you OK it), packaging (.deb or AppImage), then
   ports.

## Risks

- **Scheduler.** Speed against faithfulness is the main unknown, which is why Phase 0 has a gate.
- **Timing-sensitive builds.** Electronics and filter contraptions may act differently in GPU modes.
  Classic mode is the fallback that always behaves like TPT.
- **Stacked particles.** The data model supports them, but TPT's handling of stacks is quirky, and
  matching it in parallel passes will take care.
- **Shader compile time.** The particle update is one kernel with every element inlined, and the GPU driver's
  compile time grows about with its size squared (131 s on an RTX 5080 at 173 elements, once per shader
  change; the driver caches it). Keep inlined call sites of big functions to one where possible (movement's
  many do_move calls are the next one to fold); splitting the update into several kernels per pass compiled in
  18 s but ran 14-67% slower.
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

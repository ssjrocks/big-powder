# Big Powder

A falling-sand physics sandbox in the style of [The Powder Toy](https://github.com/The-Powder-Toy/The-Powder-Toy),
simulated on the GPU so the world can be far bigger than TPT's 612×384: up to a 4K screen at one cell per
pixel, and larger zoomable worlds beyond that.

**Status:** Phases 0 to 3 are done: the GPU engine, Classic mode running The Powder Toy's own engine, TPT save
loading, an accuracy check against TPT ([docs/accuracy.md](docs/accuracy.md)), TPT's air, walls and
simulation modes, and all 194 of TPT's elements on the GPU: the water family, growing plants, lava, melting
metals, acids and bases, electronics, sensors, explosives, cloners, light, ray emitters, wifi, EMP, lightning,
portals, pipes, pistons, tron bikes, stick men and the Game of Life. In Phase 4, TPT's renderer came to
the GPU (every element's graphics, fire glow, flares, glow and blur, the air displays and all 13 display presets),
matching TPT's own picture pixel for pixel. So did TPT's Newtonian gravity (black and white holes, gravity
bombs, GRVT, GPMP, GRAV walls, the field display and the warp effect), streamlines and signs. Phase 5 added world
files and zooming out. Phase 6 brought TPT's interface over the zoomable world: its menus, every tool, signs, undo,
copy and paste, stamps, the renderer options and settings windows, and the rest of TPT's keys, with TPT's prefs
kept between runs. Phase 7 (extras) is next. A full 4K world runs at 80–130 ticks/s on an RTX 5080 depending on the
scene, and no particles are lost or duplicated.
- Design and phases: [PLAN.md](PLAN.md)
- Phase 0 numbers: [docs/phase0-results.md](docs/phase0-results.md)

![mixed scene](docs/images/phase0-mixed.png)

## Download

Releases for Linux (x86_64) are on the [releases page](https://github.com/ssjrocks/big-powder/releases): unpack
the tarball anywhere you can write to and run `big-powder` in it. It needs a Vulkan 1.3 GPU and driver and, on
Ubuntu 26.04, these packages (most desktops have them already):

```bash
sudo apt install libsdl3-0 libvulkan1 libfftw3-single3 libpng16-16t64 libjsoncpp26 libbz2-1.0 libzstd1 libcurl4t64 libssl3t64
```

**Updates.** At start Big Powder asks GitHub for the latest release. When there is a newer one, a notification
appears at the bottom of the world, as in TPT; clicking it shows the release's notes and, if you agree,
downloads the tarball, checks it against the release's SHA-256 and puts it in place of the old files, then
starts again. Nothing is installed without that click.

**Crash and problem reports.** Release builds send a report to the author's server:
- when Big Powder crashes (sent the next time it starts);
- when you press **Report a problem** in the settings window;
- when an update fails.

A report holds:
- Big Powder's version and its recent messages (what it prints, such as loaded saves);
- for a crash, where in the program it happened;
- for a problem report, your note and the game's state: mode, world size, scene, the settings and display, GPU
  name, screen size and frame rate;
- your PC's maker and model, CPU, Linux version, kernel, and a random id made when it was first installed.

No saves, files or personal data are sent. Builds from source send nothing unless they are configured with
`-DAPP_REPORTS_URL=...`.

## Building (Linux)

Needs a Vulkan 1.3 GPU and driver. The Powder Toy's own simulation (for Classic mode and accuracy checks) is
vendored in `third_party/tpt` and built by its Meson build automatically; SDL2 is only needed for its configure step.

```bash
sudo apt install cmake ninja-build meson libsdl3-dev libsdl2-dev libvulkan-dev vulkan-validationlayers \
    libfftw3-dev libpng-dev libjsoncpp-dev libbz2-dev libcurl4-openssl-dev zlib1g-dev libssl-dev
tools/get-slang.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running

```bash
./build/big-powder                       # Screen mode: GPU engine, 3840x2160
./build/big-powder --mode classic        # Classic mode: The Powder Toy's own engine, 612x384
./build/big-powder --load some-save.cps  # open a TPT save (in GPU modes it is placed as a stamp)
./build/big-powder --load world.bpw      # open a saved GPU world (at its own size)
```

**Drawing and view**

| Action | Control |
|---|---|
| Choose tools | TPT's menus: hover a section on the right, then click a tool in the palette at the bottom with the left, right or middle button to put it on that button |
| Draw | Left drag with the left button's tool, right drag with the right button's (DUST and the eraser to start with; STKM, STKM2 and FIGH: one per click). Holding still keeps drawing, as in TPT |
| Lines, boxes, fills | As in TPT: Shift+drag draws a line, Ctrl+drag a box, Ctrl+Shift+click a flood fill; Alt snaps lines and boxes to 45° |
| Brush | Mouse wheel or `[` `]` sizes it (with Ctrl only its height, with Shift only its width; Alt+`[` `]` one pixel at a time); Tab: circle, square, triangle, then any `.ptb` brushes in `~/.local/share/big-powder/Brushes` |
| Replace mode | `;` or Insert: draw over what is there (Ctrl+Alt+click an element to replace only that one, as in TPT); Ctrl+`;` or Delete: specific delete (only that element is erased) |
| Tools | The Tools menu has TPT's simulation tools (HEAT, COOL, AIR, VAC, PGRV, NGRV, MIX, CYCL, AMBM, AMBP, WIND; Shift makes them 10 times stronger, Ctrl 10 times weaker), the property tool (PROP: a window asks what to set) and the sample tool (SMPL: click a particle to take its element; Shift+click opens the property window with its value) |
| Decoration | `B` (or the deco menu at the bottom of the right bar): the deco tools (SET, CLR, ADD, SUB, MUL, DIV, SMDG) on the mouse buttons of their own, TPT's colour presets along the bottom, and the colour button in the middle for TPT's colour picker; SMPL there takes a colour from the picture. Ctrl+`B` shows or hides decorations |
| Custom Game of Life | CUST in the Life menu: a name and a rule (such as `B36/S23`) and two colours; ctrl+shift+right click a custom type to remove it |
| Signs | SIGN in the Tools menu: click anywhere for a new sign (its text can use TPT's `{t}`, `{p}`, `{type}`... and colour codes), on one to edit, move or delete it; a button sign `{b|text}` sparks its spot when clicked with any other tool, and `{t:id|text}` opens that forum thread |
| Element search | `E`, or the magnifier button above the menus: type part of a name, description or menu; Enter takes the first match, a click takes one for that mouse button |
| Undo | Ctrl+`Z` goes back to before the last press (or paste, cut, clear, reload), five times at most; Ctrl+`Y` or Ctrl+Shift+`Z` goes forward again |
| Copy, cut, paste | Ctrl+`C` or Ctrl+`X`, then drag out an area (Shift as you let go: without the air); Ctrl+`V` shows it on the mouse: the arrow keys move it a pixel at a time, `R` turns it, Shift+`R` mirrors it, Ctrl+Shift+`R` flips it, a click places it (with Shift: without the air). A right click lets go |
| Stamps | `S` (Ctrl+`S` while STKM2 is out), then drag out an area: a stamp in `~/.local/share/big-powder/stamps` (a TPT `.stm` up to 612×384; a bigger one is our own format). `L` places the last one, `K` shows them all to choose from, search, rename or delete |
| Favourites | Ctrl+Shift+click a tool to add it to the Favorites menu (or take it out); Ctrl+Shift+right click takes it out |
| Pan | Middle drag, or Alt+drag |
| Zoom | Hold `Z` and turn the wheel |
| Buttons | Along the bottom, as in TPT: reload, save (in `~/.local/share/big-powder/saves`: Classic as a TPT `.cps`, GPU worlds as a `.bpw` world file), clear, settings, renderer options (TPT's window: each render, display and colour mode and the presets, with the world live above it), pause; quick options on the right: sand effect, gravity field, decorations, Newtonian gravity, ambient heat |
| Settings | TPT's settings window: heat, Newtonian gravity, ambient heat, water equalisation, air mode, the ambient air's temperature, pressure and velocity, vorticity, convection, gravity mode (Custom asks for a direction), edge mode, temperature scale, the simulation's and the drawing's framerate caps, interface scale, fullscreen, fast quit, sticky categories, include pressure, perfect circle brush, the deco tools' colour space. They are kept in `~/.local/share/big-powder/powder.pref` (TPT's format), with the display, the decoration colour, favourites and custom Game of Life types |

**Other keys** (TPT's; F1 in the game lists them)

| Key | Action |
|---|---|
| Space | Pause |
| `F` | Step one frame (and pause); `.` steps without pausing |
| `0`–`9` | Display presets (Shift+`6` and Shift+`0` two more, Shift+`1` with the debug HUD one more) |
| `G` / Shift+`G` | Grid: one cell wider apart / narrower |
| Ctrl+`F` | Highlight the left button's element (or what the property tool sets) |
| `H` | Hide or show the HUD; `D` or F3 the debug HUD; F1 or Ctrl+`H` the introduction |
| `P` or F2 | Screenshot: the whole world as `screenshot <date>.png` in `~/.local/share/big-powder` |
| `=` / Ctrl+`=` | Reset the air / reset sparks |
| `I` | Invert the air's pressure and velocity |
| Ctrl+`U` | Reset ambient heat |
| `M` | Switch between Classic and the GPU engine |
| Ctrl+`P` | The property tool (Ctrl+Shift+`P`: on the right button) |
| Ctrl+`E` / `W` / `Y` / `U` | Edge mode / gravity mode / air mode / ambient heat (while STKM2 is out, Ctrl+`W`) |
| `N` | Newtonian gravity on/off |
| Ctrl+`G` | Gravity field display on/off (the GRAV wall tool shows where Newtonian gravity reaches) |
| Ctrl+`B` | Decorations on/off |
| Arrow keys | Steer STKM: left, right, up jumps, down uses its element |
| `A` `D` `W` `S` | Steer STKM2 the same way |
| Drop a `.cps`/`.stm` file | Load it where you drop it |
| F6 | Next scene |
| Ctrl+`R` or F5 | Reload scene |
| Home | Fit the view |
| F11 | Fullscreen |
| Esc or `Q` | Quit (asks first, as TPT does) |

Useful options:

- `--present mailbox`: uncapped frame rate (while the settings' rendering cap follows the display).
- `--tps N`: simulation ticks per second (default 60, like TPT; the settings' simulation cap otherwise).
- `--size WxH`: world size.

Benchmarks and the accuracy check against TPT ([docs/accuracy.md](docs/accuracy.md)):

```bash
./build/big-powder --bench --scene all --batch 2
./build/big-powder --compare --scene pile,dam,sink --ticks 600 --interval 150
```

## Licence

GPL-3.0-or-later. This project ports code from The Powder Toy, which is GPL-3.0.

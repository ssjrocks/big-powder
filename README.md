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
bombs, GRVT, GPMP, GRAV walls, the field display and the warp effect), streamlines and signs. Phase 4 is done;
the huge world (Phase 5) is next. A full 4K world runs at 80–130 ticks/s on an RTX 5080 depending on the scene, and no particles are lost or
duplicated.
- Design and phases: [PLAN.md](PLAN.md)
- Phase 0 numbers: [docs/phase0-results.md](docs/phase0-results.md)

![mixed scene](docs/images/phase0-mixed.png)

## Building (Linux)

Needs a Vulkan 1.3 GPU and driver. The Powder Toy's own simulation (for Classic mode and accuracy checks) is
vendored in `third_party/tpt` and built by its Meson build automatically; SDL2 is only needed for its configure step.

```bash
sudo apt install cmake ninja-build meson libsdl3-dev libsdl2-dev libvulkan-dev vulkan-validationlayers \
    libfftw3-dev libpng-dev libjsoncpp-dev libbz2-dev
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
| Element search | `E`, or the magnifier button above the menus: type part of a name, description or menu; Enter takes the first match, a click takes one for that mouse button |
| Pan | Middle drag, or Alt+drag |
| Zoom | Hold `Z` and turn the wheel |
| Buttons | Along the bottom, as in TPT: reload, save, clear, renderer options (display presets for now), pause; quick options on the right: gravity field, decorations, Newtonian gravity, ambient heat |

**Other keys**

| Key | Action |
|---|---|
| Space | Pause |
| `.` | Step one tick |
| `V` | Display mode: TPT's display presets (Fire, Pressure, Velocity, Heat, Fancy, Blob, Persistent...) |
| `C` | Colour passes (GPU modes) |
| `M` | Switch between Classic and the GPU engine |
| Ctrl+`P` | The property tool (Ctrl+Shift+`P`: on the right button) |
| Ctrl+`E` / `W` / `Y` / `U` | Edge mode / gravity mode / air mode / ambient heat (while STKM2 is out, Ctrl+`W`) |
| `N` | Newtonian gravity on/off |
| Ctrl+`G` | Gravity field display on/off (the GRAV wall tool shows where Newtonian gravity reaches) |
| Ctrl+`B` | Decorations on/off |
| Arrow keys | Steer STKM: left, right, up jumps, down uses its element |
| `A` `D` `W` `S` | Steer STKM2 the same way |
| Ctrl+S | Save, in `~/.local/share/big-powder/saves`: Classic as a TPT `.cps`, GPU worlds as a `.bpw` world file |
| Ctrl+Shift+S | GPU worlds: save the 612×384 area around the mouse as a TPT `.cps`, to open in The Powder Toy |
| Drop a `.cps`/`.stm` file | Load it where you drop it |
| F6 | Next scene |
| `R` | Reload scene |
| `-` / `=` | Simulation speed |
| Home | Fit the view |
| F11 | Fullscreen |
| Esc | Quit |

Useful options:

- `--present mailbox`: uncapped frame rate.
- `--tps N`: simulation ticks per second (default 60, like TPT).
- `--size WxH`: world size.

Benchmarks and the accuracy check against TPT ([docs/accuracy.md](docs/accuracy.md)):

```bash
./build/big-powder --bench --scene all --batch 2
./build/big-powder --compare --scene pile,dam,sink --ticks 600 --interval 150
```

## Licence

GPL-3.0-or-later. This project ports code from The Powder Toy, which is GPL-3.0.

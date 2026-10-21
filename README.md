# Big Powder

A falling-sand physics sandbox in the style of [The Powder Toy](https://github.com/The-Powder-Toy/The-Powder-Toy),
simulated on the GPU so the world can be far bigger than TPT's 612×384: up to a 4K screen at one cell per
pixel, and larger zoomable worlds beyond that.

**Status:** Phases 0 to 2 are done: the GPU engine, Classic mode running The Powder Toy's own engine, TPT save
loading, an accuracy check against TPT ([docs/accuracy.md](docs/accuracy.md)), and TPT's air, walls and
simulation modes. Phase 3 (elements) is under way: 131 of TPT's elements run on the GPU so far, including the
water family, growing plants, lava, melting metals, acids and bases, electronics, sensors, explosives and
cloners. A full 4K world runs at 80–130 ticks/s on an RTX 5080 depending on the
scene, and no particles are lost or duplicated.
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
```

**Drawing and view**

| Action | Control |
|---|---|
| Draw | Left drag |
| Pan | Right or middle drag |
| Zoom | Mouse wheel |
| Brush size | Shift+wheel, or `[` and `]` |
| Select element | `1` DUST, `2` WATR, `3` OIL, `4` FIRE, `5` STNE, `6` METL, `0` erase |
| Any element | PageUp / PageDown (every TPT element in Classic) |

**Other keys**

| Key | Action |
|---|---|
| Space | Pause |
| `.` | Step one tick |
| `V` | Display mode (TPT's display presets in Classic, heat view in GPU modes) |
| `C` | Colour passes (GPU modes) |
| `M` | Switch between Classic and the GPU engine |
| `L` | Wall brush on/off (PageUp/PageDown picks the wall type, `0` erases walls) |
| `E` / `W` / `Y` / `U` | Edge mode / gravity mode / air mode / ambient heat |
| Ctrl+S | Save (Classic, as a TPT `.cps` in `~/.local/share/big-powder/saves`) |
| Drop a `.cps`/`.stm` file | Load it where you drop it |
| Tab | Next scene |
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

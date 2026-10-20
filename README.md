# Big Powder

A falling-sand physics sandbox in the style of [The Powder Toy](https://github.com/The-Powder-Toy/The-Powder-Toy),
simulated on the GPU so the world can be far bigger than TPT's 612×384: up to a 4K screen at one cell per
pixel, and larger zoomable worlds beyond that.

Status: early. Phase 0 (engine spike) is in progress. See [PLAN.md](PLAN.md) for the design and phases.

## Building (Linux)

Needs a Vulkan 1.3 GPU and driver.

```bash
sudo apt install cmake ninja-build libsdl3-dev libvulkan-dev vulkan-validationlayers glslc
tools/get-slang.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Licence

GPL-3.0-or-later. This project ports code from The Powder Toy, which is GPL-3.0.

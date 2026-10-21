# Accuracy against The Powder Toy

The GPU engine is checked against TPT v100.1.400 itself. TPT's real simulation code is built into this app
(Classic mode, `third_party/tpt`). The comparison tool runs the same starting state through both engines and
measures how far the results drift apart:

```bash
./build/big-powder --compare --scene pile,dam,sink,layers,rain,mixed --ticks 600 --interval 150
```

## How it's measured

- **Same start.** The scene is built in TPT with TPT's own `create_part`, so every particle gets TPT's
  defaults. The GPU engine starts from exactly those particles.
- **Distance.** At each checkpoint, the share of particles that would have to move to make the two
  (element, 8×8 px bin) distributions identical, i.e. their total variation distance. 0% = the same; 100% =
  nothing in common.
- **Baselines.** Falling sand is chaotic, so two TPT runs never match exactly either. Two baselines show how
  much TPT varies on its own:
  - *TPT seed*: TPT again with another random seed.
  - *TPT order*: TPT again with the same particles created in a random order. TPT updates particles in id
    order, i.e. creation order, so this is a difference TPT users see in practice.
- **Goal.** The GPU engine is as accurate as it can be when its distance from TPT is about the size of these
  baselines.

## Results (2026-10-21, 612×384, 600 ticks, mean over checkpoints)

| Scene | What it tests | TPT seed | TPT order | GPU (7×7) |
|---|---|---|---|---|
| pile | dust falling into a heap | 2.4–2.5% | 4.0–4.1% | 4.0–4.1% |
| dam | water released into a tank | 0.6–0.7% | 1.1–1.3% | 1.5–2.1% |
| sink | stone sinking through water | 1.7–1.8% | 1.9–2.0% | 2.1–2.2% |
| layers | oil rising through water | 2.3% | 17% | 15% |
| rain | water drops falling | 3.6–3.7% | 4.0–4.4% | 3.8–3.9% |
| mixed | oil fire, dust, stone, water | 9.5–9.6% | 10.5–10.7% | 24.5–25% |

Ranges are over two runs with different seeds.

- All scenes except `mixed` are at TPT's own level of variation.
- `mixed` is higher because TPT turns burning oil into GAS and fire into SMKE, and those elements aren't
  ported yet.

## What the comparison found and fixed

1. **Air solver.**
   - Before air was ported, falling dust was 9–21% away from TPT.
   - Running TPT with its air updates switched off brought it back within TPT's own variation, which proved
     the cause.
   - TPT's `Air::update_air` is now ported (`shaders/air.slang`), and the pile is at 3.5–4.7%.
2. **Serial check.**
   - `--serial` runs the GPU port one particle at a time in id order, exactly like TPT's update loop.
   - It matches TPT to within TPT's seed noise (0.5–0.8% on the dam), so the rules themselves are ported
     correctly.
   - Any remaining difference comes only from updating in parallel.
3. **Colour grid size.**
   - Liquids are sensitive to the size of the colour grid.

     | Grid | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 10 | 16 |
     |---|---|---|---|---|---|---|---|---|---|---|
     | dam distance | 4.5% | 3.3% | 2.0–2.4% | 1.4–1.6% | 3.9–4.2% | 3.6% | 1.5–2.1% | 1.9–2.2% | 4.9% | 3.5% |

   - Grid sides that divide TPT's liquid scan ranges (30 and 10 px) do worst. This is an observed pattern,
     not yet explained.
   - **7×7 is the default.** It is close to TPT in every scene, and strictly race-free for reactions that
     reach up to 3 px.
   - Its cost at 4K: a full tank of water takes 11.6 ms per tick (86 ticks/s), sand 8.2 ms, the mixed scene
     5.8 ms.
4. **Things that did not matter:**
   - pass order kept fixed across ticks;
   - raster pass order (worse);
   - cache-coherent `pmap` reads;
   - air coupling.

## Air, walls and simulation modes (Phase 2)

**Direct field check (`--air-test`).**
- Both engines get the same pressure blob, air jet and (with `--aheat`) hot spot, with no particles, and the
  fields are compared cell by cell.
- After 200 ticks, pressure and velocity agree to within 0.0001 and ambient heat to within 0.013 K on a
  1,500 K hot spot. That is float rounding: the air solver and ambient heat are exact ports.
- Getting heat that close needed one subtle TPT detail. `update_airh` writes air convection in place during
  its raster-order loop, so cells it has already passed are read with this tick's convection included. This
  is reproduced with a separate convection pass.

**Scenes with walls and modes (600 ticks, 7×7).**

| Test | TPT seed | TPT order | GPU |
|---|---|---|---|
| `walls` (wall tank, shelf, liquid filter, absorb wall) | 2.7% | 3.0% | 4.6% |
| `detector` (detector wall opening an E-wall gate) | 8–32% | 3–8% | 6–18% |
| `open`, edge void | 6–9% | 9–12% | 9–13% |
| `open`, edge solid | 4–9% | 4–11% | 8–11% |
| `open`, edge loop | 7–19% | 9–22% | 9–22% |
| `open`, gravity off | 0% | 0% | 0% |
| `open`, radial gravity | 0.7–1.4% | 1.0–2.1% | 1.6–2.3% |

- **`detector`** is chaotic in TPT itself: the gate's timing depends on exactly when the water lands, so even
  two TPT runs differ a lot.
- **Void and solid edges.** In `open`, dust falls freely through open air, and the GPU engine falls a little
  slower than TPT.
  - With air coupling switched off in both engines (`--no-air-coupling`), it matches TPT's own variation.
  - The difference therefore comes from parallel updates interacting with the moving air, not from the air
    solver, which matches exactly.

## Elements (Phase 3)

Each element batch is checked with a scene that exercises it.

| Scene | What it tests | TPT seed | TPT order | GPU (7×7) |
|---|---|---|---|---|
| `boil` | water boiling off a red-hot plate, steam condensing, ice melting, salt dissolving | 1.6–19% | 5–31% | 4–23% |
| `mixed` (with water family, smoke, gas) | oil fire over water | 9.6% | 10.5% | 11.7% (was 24.5% before smoke, gas and steam) |
| `plants` | seeds growing into trees on sand, PLNT spreading through water, VINE climbing WOOD | 4.8% | 4.7–4.9% | 4.2–4.4% |
| `sponge` | sponges soaking rain and steaming on a hot plate, PLNT turning CO2 into O2 | 2.3–2.4% | 2.4–2.5% | 2.3–2.4% |
| `coal` | fire fed by O2 setting coal smouldering; wood charring on a hot plate | 15.1–15.3% | 15.7–18% | 15.6% |

The plant scenes run 2,000 ticks (checkpoints every 250); ranges are over two seeds.

After 900 ticks of `boil` the element counts line up:

| Element | TPT | GPU |
|---|---|---|
| ICE | 15,144 | 15,225 |
| SLTW | 9,696 | 9,628 |
| WTRV | 8,330 | 9,087 |
| WATR | 36,558 | 35,055 |

**Creating particles.** Plants grow by creating new particles (`create_part`) in the middle of a tick. The
GPU engine hands out ids from a free list built at the start of each tick.

- In TPT a new particle is usually updated later in the same frame, because it gets a higher id than the
  particle that made it. In the GPU's colour passes it waits until the next tick.
- The serial reference mode (`--serial`) allocates ids exactly as TPT does (ids freed this frame first, then
  the lowest unused one) and so does update new particles in the same tick. It matches TPT within TPT's own
  variation on `plants` (7.4% after 2,000 ticks against 7.2–7.3%).
- The parallel engine is just as close, so the one-tick delay doesn't show at this level.

## Known limits

- **Liquids.** The GPU engine remains slightly behind TPT at levelling out, and a little foamier.
- **Particle order.** TPT updates in id order; after loading a save that is screen order, top to bottom. No
  parallel scheme reproduces that exactly. The target is to be as close to TPT as TPT is to itself under a
  different order.

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
   - raster pass order (worse overall, see "Pass order" below);
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

**Lava and melting** (600 ticks, checkpoints every 150; dT = mean temperature difference per element and
8 px bin, a column the comparison gained with this batch):

| Scene | What it tests | TPT seed | TPT order | GPU (7×7) |
|---|---|---|---|---|
| `lava` | lava on a metal shelf over sand, thermite into iron, water on lava, rust | 12.9% (dT 50 K) | 13.7% (dT 60 K) | 13.8% (dT 64 K) |
| `thermite` | a thermite strip burning into an iron block | 3.3% (dT 18 K) | 24.5% (dT 103 K) | 27.5% (dT 117 K) |
| `heat` | heat conduction along metal bars, no melting | 0% (dT 0 K) | 0% (dT 36 K) | 0% (dT 35 K) |

- The serial reference mode matches TPT exactly on `heat` (dT 0.0–0.1 K) and within TPT's variation on
  `lava`, so the ports are right.
- `thermite` shows how much update order matters in TPT itself. With particles numbered top to bottom (as
  after loading any save), 2,150–2,570 iron is left after 600 ticks; numbered in random order, 0–330. Heat
  conducts down the chain of particles within one frame in screen order, so it spreads out before it can
  melt the iron. The GPU engine (0–135 left) behaves like the random order.

**Chemistry** (600 ticks, checkpoints every 150, two seeds):

| Scene | What it tests | TPT seed | TPT order | GPU (7×7) |
|---|---|---|---|---|
| `chem` | acid on stone, base into acid, CO2 over water, hydrogen and oxygen lit by fire (rooms divided by walls) | 9.6–10.2% (dT 3.0–3.5 K) | 10.8–12.1% (dT 5.5 K) | 11.6–12.9% (dT 5.3–6.1 K) |
| `neutral` | base poured into acid: neutralising, and heating as the acid eats the salt water | 19.4–20.2% (dT 14–16 K) | 21.0–22.1% (dT 17–22 K) | 21.6–25.5% (dT 18 K) |

- Both scenes are chaotic in TPT itself: in `neutral`, between 66 and 247 BASE is left after 800 ticks
  depending only on the seed or particle order. The GPU engine lands in that range (109–146; serial mode
  186–379).

**More liquids and powders** (`rooms`: GEL soaking rain next to a sponge, mercury on a hot plate, FRZZ dropped
into water, a quartz crystal growing in salt water, virus on sand next to warm yeast; 600 ticks, two seeds):
TPT seed 1.3–1.5%, TPT order 1.4%, GPU 1.5% (dT 0.8–1.6 K, TPT 0.7–2.2 K). After 2,000 ticks the quartz
crystal has grown to 80–140 particles on the GPU and 82–123 in TPT; mercury ends at 1,220–1,232 (TPT random
order 1,205–1,228) and in serial mode at 1,177–1,178 (TPT screen order 1,178–1,188).

**Electronics** (`circuit`: four battery-powered strips separated by walls: a METL wire into water ending in
IRON with GAS above it for sparks to ignite; PSCN -> NSCN -> METL -> SWCH -> METL; a long INST line; a
WireWorld loop; 600 ticks, two seeds): TPT seed 3.7–3.9%, TPT order 3.8–4.0%, GPU 4.1–4.4% (dT 3.5–3.8 K,
TPT 2.7–3.9 K). The INST line is sparked all at once at the same tick in both engines.

**Sensors and explosives** (`explosives`: six walled rooms with a battery-lit FUSE, a fire-lit IGNT cord running
to TNT, a BOMB dropped on stone, a hot block next to TSNS wired to IGNT, DEST dropped on brick, LNTG poured on
C5; 600 ticks, two seeds): TPT seed 18.2–20.6%, TPT order 17.8–22.2%, GPU 22.1–24.1%.

- Whether a burnt-out IGNT cord sets off the TNT it touches is a coin flip in TPT itself (`tntcord`: lit in
  2 of 3 TPT runs with one seed, in none with another), so that scene is all or nothing; the GPU lands on
  both outcomes too.
- **Update order and one-tick windows.** An IGNT lights when a burning neighbour is at exactly life 1. TPT's
  order never changes, so a cord always burns through; the GPU reshuffles the colour passes each tick, which
  skipped that tick about one time in four and broke cords (44–48 of 50 IGNT left unburnt). A burnt-out lit
  neighbour (life 0, gone next tick) now also counts, at most one tick late, and cords burn through.
- A fixed pass order (`--pass-order 3`) would also keep neighbours in the same order every tick, but over all
  20 scenes it was no closer to TPT (mean distances summed: 229.9 against 227.0), so the default stays random.
- BOMB explosions reach 9 px, beyond what the colour passes keep apart, so they run one at a time after the
  passes (as do INST and BANG flood fills). A bomb destroyed by an earlier explosion doesn't explode, as in
  TPT; one that has asked to explode stays in place until then.

**Cloners, voids and other special elements** (`specials`: CLNE picking up water over a VOID floor, sand raining
onto a black hole, CONV under rain, a PSCN-powered PCLN cloning dust that sparks set alight, a shield growing
from sparks; 600 ticks, two seeds): TPT seed 10.6–13.7%, TPT order 11.4–11.7%, GPU 10.7–13.6% (dT 1.1–2.6 K,
TPT 1.4–3.0 K). The void, black hole, converter and all four shield layers end with exactly TPT's counts.

**Light** (`light`: a CLNE turned into a light source by a photon touching it, shining on a glass block, a metal
wall, a GLOW pool, quartz and an RSST blob; 600 ticks, two seeds): TPT seed 7.0–9.8%, TPT order 8.1–11.5%,
GPU 7.7–9.4% (dT 30–36 K against TPT's 19–98 K). Photons live in their own map (TPT's photons), pass
through or reflect off what they meet (with TPT's boundary-tracing surface normals), refract into and out of
glass by wavelength, and gain in GLOW. TPT's own photon count after 400 ticks swings from 186 to 2,533 with
the seed and particle order; the GPU's lands in that range.

**Nuclear** (`nuclear`: neutrons in a block of PLUT, neutrons in a DEUT pool, electrons over water and a METL
bar, POLO and URAN; 600 ticks, three seeds): TPT seed 30.8–31.6%, TPT order 24.4–25.9%, GPU 30.6–36.1%. Fission
chains are extremely sensitive: whether the DEUT pool goes off, and how much PLUT is left (1,148–1,592 in TPT,
1,089–1,460 on the GPU), varies from run to run in TPT itself.

**Exotic matter and filters** (`exotic`: light through a FILT column, antimatter on stone, a singularity in
sand, an EXOT pool, VIBR on a hot plate, tungsten above its melting point; 600 ticks, two seeds): TPT seed
7.0–7.3%, TPT order 6.8–7.0%, GPU 6.6–7.1% (dT 7.8–12.4 K, TPT 5.2–8.5 K). How much sand a singularity eats
varies a lot in TPT itself (`sing`: 1,685–2,881 of 3,700 left after 400 ticks; GPU 2,395–2,854).

**Fixed along the way: burning gas.** A strip of GAS lit by fire (`gasfire`) burned almost twice as fast on the
GPU (1,877 FIRE after 100 ticks against TPT's 1,075), in serial mode too. TPT lets GAS and NBLE push the air
pressure only up towards 3.5 (their HotAir), while the GPU engine added it without limit like other
elements; higher pressure makes gas catch fire more easily. With that fixed, `gasfire` is at 53.8–55.7%
against TPT's own 52.2–55.0%.

**Pass order.** Running the colour passes in raster order instead of a random order reproduces TPT's
screen-order thermite (2,320–2,700 iron left), and heat conduction comes within 5 K of TPT instead of 35 K.
But it breaks movement, and its lava and thermite temperatures end up further from TPT:

| Scene | random passes (default) | raster passes |
|---|---|---|
| pile | 4.0% | 19.1% |
| dam | 1.5% | 10.0% |
| walls | 4.7% | 9.7% |
| rain | 3.9% | 6.7% |
| layers | 15.2% | 6.8% |
| lava | 13.8% (dT 64 K) | 20.3% (dT 142 K) |
| thermite | 27.5% (dT 117 K) | 15.1% (dT 162 K) |
| heat | dT 35 K | dT 5 K |

Random pass order matches TPT best overall, so it stays the default (`--pass-order 1` selects raster).

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

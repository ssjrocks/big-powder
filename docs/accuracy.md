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

**Stacking** (`stacking`: piles of 4, 8, 20, 50, 200 and 2,000 METL in single cells and a SAND pile of 50; 600
ticks, two seeds): GPU 0.0–0.2%, TPT seed 0.0–0.1%, TPT order 0.0–0.3%. In both engines every pile of more than 5
METL collapses into NBHL (which TPT names "BHOL"), the pile of 4 stays, and the SAND spreads out before it can
collapse.

- TPT counts the particles in every cell each tick (`pmap_count`). Here prepare counts them only in cells
  where a second particle turns up, and lists those cells, so a tick without stacking costs nothing extra.

**Pasting saves** (`--check-save FILE`: pastes a save into an empty GPU world as the viewer does, and checks it
against TPT's own load). On saves made from the `soap` scene after 120 ticks, the `pipes` scene and the
`stacking` scene, every particle lands in its cell, all of SOAP's 220 links join the same cells as in TPT,
and the air (pressure, velocity, ambient heat) is identical. Running on, the soap save stays at 6.9–7.4% from
TPT and the pipes save at 2.5–2.6%, as close as their scenes are.

- As TPT's `Simulation::Load` does: particles already where the save puts one are removed, stacked particles
  in the save are kept, SOAP's links (particle ids) are renumbered, and a stacking check follows.
- A save's simulation settings (gravity mode and so on) are applied by TPT's game, not by its loader; a save
  pasted into a GPU world keeps the world's settings.

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

**Force elements** (`force`: water past ACEL and DCEL, a warm RPEL block in falling dust, DMG dropped on sand
over glass and wood, a HEAC bar between a hot and a cold block, CNCT poured into a pile; 600 ticks): TPT seed
11.4–18.8%, TPT order 12.5–19.1%, GPU 13.3–20.1%. In serial mode the temperatures match TPT exactly and the DMG
blast breaks exactly as much glass and wood as TPT's.

**Beams and wireless** (`rays`: an ARAY beam through a FILT column onto a METL bar with dust falling through it
and a PSCN-fired erasing beam across it; a CRAY shooting sand; a DRAY copying a stone and gold pattern; WIFI on
one channel driving electrolysis and on another that stays off; an INST-fired beam through METL bars and a STOR;
600 ticks, two seeds): TPT seed 4.9–5.3%, TPT order 5.5–5.8%, GPU 6.2–7.1% (serial 5.7–5.8%), dT within 0.3 K
of TPT's. The beams, the 888 sand CRAY shoots and the DRAY copies end with TPT's counts.

- TPT casts a beam inside the emitter's update. A beam can cross the whole world, so the GPU casts beams after
  the passes, one thread each (serial mode: one at a time, in update order). What a beam creates or sparks is
  therefore updated from the next tick on, as in TPT for particles with lower ids than the emitter; and beams
  cast in the same tick may meet in either order.
- CRAY passes its ctype's upper bits to the new particle only for FILT (its mode); LIGH, LIFE and TESC use
  them too and come later.

**EMP** (`emp`: eight EMP set off by a spark beside three battery-sparked PSCN lines with METL, BMTL, SWCH,
DLAY, WIFI, ARAY and GLAS within reach; 600 ticks, four seeds): TPT seed 20.9–37.7%, TPT order 24.4–37.4%, GPU
26.5–42.6%. The sparked lines also fire the ARAY next to them, and TPT's own BRAY count varies from 2,693 to
4,684 (GPU 2,640–4,271). What is left of the SWCH, WIFI and ARAY that EMP breaks lands in TPT's range;
broken electronics plus sparks (BREL conducts) total 1,058–1,099 on the GPU against TPT's 1,027–1,106.

- TPT applies EMP after the tick by walking every spark (and lit WIRE, switching SWCH) in id order and
  damaging the electronics within 2 px. The GPU does it per particle instead: each one gathers the sparks
  around it and takes their effects in the same id order, with TPT's probabilities for the number of EMP set
  off. A SWCH spark that an earlier spark breaks still counts here (in TPT it doesn't), and a spark under
  another particle only affects itself. The screen flash (`emp_decor`) comes with the display modes.

**Electrodes and lightning** (`electric`: battery-fed ETRD as two single electrodes, a pair with INSL halfway,
and two ETRD lines facing each other; a Tesla coil over wood, DEUT and METL; lightning striking a METL plate,
gunpowder, water and wood; 600 ticks, two seeds): TPT seed 19.8–20.9%, TPT order 13.1–27.9%, GPU 30.9–35.6%
(serial 17.7–20.4%). Whether the Tesla coil's room catches fire decides most of that: lightning melts the
plate under the coil, and lava dripping on the wood floor sets it alight in 9 of 18 TPT runs (six seeds) and in
5 of 6 GPU runs.

- ETRD lines facing each other keep arcing back and forth. TPT's plasma after 300 ticks over six seeds:
  3,609–4,365; GPU 3,545–4,344 (serial 4,206–4,446). Arcs are made during the colour pass, as in TPT, however far
  apart the electrodes are: the spark claims its target with an atomic swap, and an electrode updated in the
  same pass is skipped (its own update may be running). Made after the passes instead, every arc came too late:
  the electrodes' own sparks took the free ETRD first, the arcs fell into step and in two of six seeds they
  died out altogether.
- TESC makes lightning as often as in TPT (eight coils, 300 ticks, three seeds: lightning at 26–39 of 150
  checkpoints, TPT 24–40).
- LIGH draws each new stretch of lightning after the passes (it can reach 130 px), so a bolt runs one tick
  behind TPT, where a new segment with a higher id than its parent is drawn in the same frame. It heats what
  it strikes just as much: a METL plate under ten bolts is at the same temperature one tick later than in TPT.
- Brush sizes for LIGH and TESC come from the brush radius; TPT's limit on how often the brush makes lightning
  comes with the UI.

**Portals and more** (`portals`: sand poured onto a PRTI and a battery's sparks coming out of a PRTO in the next
room, water through a hotter channel, photons into a PRTI, LITH dropped into water and a LITH battery between
PSCN and NSCN, PTNM in H2 and O2 and under smoke, MORT, LOVE and LOLZ; 600 ticks, two seeds): TPT seed
35.7–43.4%, TPT order 35.6–42.2%, GPU 42.1–44.2% (serial 41.5–42.3%). All the sand comes through the portal as in
TPT, and LOVE's hearts and LOLZ's letters end with exactly TPT's counts.

- TPT keeps what goes into portals in one store per channel. Portals of a channel can be updated in the same
  colour pass anywhere in the world, so a portal claims a slot in the store with an atomic swap before it
  writes or reads it; when two take from or put into the same channel in one pass, which slot each gets
  depends on timing.
- LOVE and LOLZ are redrawn in a pass of their own before the update passes, one thread per 9x9 block, as
  TPT's BeforeSim does (it runs only on ticks where there are any).
- SOAP's bubble links and stick men going through portals come with those elements.

**Game of Life** (`life`: random soups of four rulesets, GOL, HLIF, DANI and Brian 6 with three states, and a
glider heading for a sand pile; 600 ticks, two seeds): TPT seed 0.1–0.3%, TPT order 0.1–0.2%, GPU 0.1–0.3%
(serial 0.1%). The same LIFE is left as in TPT (886 after 600 ticks with one seed; 938 against 942 with the other).

- TPT's SimulateGoL runs before the update passes in three passes: every LIFE at full strength is marked, each
  cell gathers the marked LIFE around it in id order (so the neighbour list, the counts and which neighbour a
  new cell takes its colours from come out exactly as in TPT's loop over particles), and dead LIFE is removed.
- What LOVE, LOLZ and the Game of Life make before the passes joins the colour lists and is updated in the same
  tick, as in TPT; the passes are sized afterwards.
- Fixed along the way: TPT cools every LIFE by 50 K a tick; without that, LIFE stayed 500 K hotter than TPT's.
- Custom rulesets run, but their colours (from TPT's custom GoL list) aren't looked up yet.

**SOAP** (`soap`: a SOAP pool under gunpowder that fire sets off, SOAP and OIL poured together, SOAP poured onto
a PRTI; 600 ticks, two seeds): TPT seed 6.1–7.7%, TPT order 6.9–8.7%, GPU 6.8–8.2%, with TPT's SOAP and OIL
counts.

- Bubble film links into chains by particle id, and the chains pull together; a link and a bubble breaking up
  along its chain can reach further than the colour passes keep particles apart, but linked particles are
  usually within 3 px, so two threads rarely touch the same one.
- TPT detaches SOAP from its chain whenever it is killed or turned into something else. Here the other end of
  the link drops it in its own update (usually later the same tick): done in every kill, it made the update
  kernel 19% bigger and a minute slower to compile.

**Pipes** (`pipes`: three pipes fed from tanks, each opening onto a wall only what it carries can pass: PIPE
carrying water with HEAC and a hot METL block touching it, PPIP carrying sand sparked on by PSCN, PPIP carrying
water paused by NSCN; 800 ticks, two seeds): TPT seed 2.9–4.4%, TPT order 3.0–4.2%, GPU 3.2–4.7% (serial
2.7–7.4%). The BRCK borders, what comes out and the steam from the heated pipe match TPT's counts.

- A particle moves at most two pipes a tick, as in TPT, whose recursion is unrolled here. TPT lets a pipe that
  was just given a particle by a pipe with a lower id skip its own push that tick; here the question is
  whether the receiving pipe's colour pass comes later in the tick (in id order in serial mode).
- PPIP's spark triggers reach the whole connected network, so they are flooded after the passes, like
  FloodINST; they take effect the next tick, as in TPT. TPT's flood fills a row again for every cell of it left
  on its stack; on one GPU thread that cost up to 7 ms a tick, so done cells are skipped (same result).
- An energy particle that bounces off an empty pipe leaves a copy of itself in it, as in TPT.

**Pistons** (`pistons`: three battery-driven pistons, one pushing a stack of stone, one lifting a FRME frame with
sand on it, one extended by PSCN and pulled back by NSCN against a BRCK block; 600 ticks, two seeds): TPT seed
0.6–1.1%, TPT order 0.5–0.8%, GPU 0.9–1.5% (serial 0.3–0.4%). Every element count matches TPT's exactly,
including the arm sections the pistons add and remove.

- TPT moves a piston and everything it pushes in the piston's own update, which can reach far along a line.
  Here a sparked piston asks during its colour pass and is moved after the passes, one at a time in the order
  they asked (in id order in serial mode). So something a piston pushed is updated at its new place only the
  next tick.

**Tron bikes** (`tron`: three rooms of TRON heads among brick pillars: ordinary ones, ones that never turn at
random, and ones whose tails never die; 600 ticks, six seeds): TPT seed 30–49%, TPT order 36–46%, GPU 39–46%
(serial 47%). Random turns make the bikes' paths diverge fast in TPT itself. TPT ends with 2,041–2,536 TRON
depending on seed and order, the GPU with 2,257–2,536.

- With every head set never to turn at random, serial mode gives exactly TPT's result over 600 ticks (0.0% on
  both seeds), so the steering, the look-ahead and the tail growth are TPT's.
- TPT makes a new head that its loop will reach later in the same tick wait a tick, so a head moves one pixel
  a tick. The colour passes never update a particle in the tick it was made, so there it doesn't wait.

**Stick men** (`stickmen`: two rooms with a brick floor; in one, three fighters walk over a brick step to STKM;
in the other, two fighters, one holding FIRE, cross a strip of hot metal to STKM2; a cloud of GRAV near the
ceiling; 600 ticks):

- Players standing still (four seeds): TPT seed 0.0–0.3%, TPT order 0.0–0.3%, GPU 0.0–0.2% (serial 0.2%). Over
  ten runs in all, TPT keeps 3–5 of the 5 fighters and the GPU 3–5; the fire the FIRE fighter shoots ends at
  0–138 particles in TPT and 0–117 on the GPU (none where it never gets close).
- Players steered (`--stickman-keys 9,1,6,2`: STKM walks left shooting water, STKM2 walks right jumping; two
  seeds): TPT seed 1.2–2.3%, TPT order 1.8–1.9%, GPU 1.9–2.2% (serial 1.5%), with as much water shot as in TPT.
- In serial mode the stick men's paths follow TPT's to within rounding for 100–400 ticks (a fighter's walk,
  STKM2 climbing the wall). Then, as between TPT's own runs, they drift apart: TPT computes part of the leg
  physics in double precision, and a foot landing a hair lower can change a step.
- TPT runs a stick man's update in its own particle update, between its heat and its movement. Here one
  thread runs all of them before the colour passes, in id order; the head then moves in its pass. STKM's
  air loss is 1, so its velocity comes out the same.
- TPT keeps track of stick men in their create and kill functions, wherever a particle is made or killed.
  Here the stick men prepare lists are checked against their slots once a tick, before the passes; one made
  during a tick isn't updated until the next anyway.

**Fixed along the way: fighter slots in the accuracy check.** The check passed a scene's ctype and tmp to TPT
for any particle that sets them. For a FIGH, tmp is the fighter slot TPT gave it, so a fighter holding FIRE
shared slot 0 with another, and was pulled across the room by the other's legs. TPT's slot is now kept.

**Fixed along the way: scene settings.** The accuracy check gave TPT only each particle's element and
temperature, so a scene's CRAY ctype or STOR contents never reached either engine. It now passes ctype and tmp
too.

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

## Drawing (Phase 4)

`--render-check` runs TPT on a scene for 100 ticks, gives the GPU engine exactly that state (particles, walls,
air) and draws it with TPT's renderer and the GPU's, three frames each (so the fire buffer builds up), for all 13
display presets, comparing pixel by pixel. Over 28 scenes (364 scene and preset pairs):

- 290 pairs match TPT exactly, every pixel. That includes `streams` and `signs` in every preset.
- The rest differ only where TPT draws at random: the flicker of sparks and flares (EMBR, DEST, BOMB, DMG, GRAV,
  LITH, SLCN), GLOW's flicker, GOLD's speckle; there 97.7–99.9% of pixels match. A few pixels more come from
  floating-point rounding (portals' orbiting dots, the heat gradient display).
- Stick men are the exception the check can't show: their legs live in TPT's player state, which isn't copied.

How it stays exact: TPT draws particle by particle in id order, and where several particles draw on the same pixel
(glows, blurs, flares, stacked particles) the order decides the result. Each pixel's operations are gathered and
put back into that order (overwrites and blends sorted, saturating adds summed between them), the fire buffer's
in-place smoothing keeps TPT's order with a wavefront, and the tables (flame, plasma, heat colours, the fire glow's
shape) come from TPT itself.

Fixed along the way: `GpuSim::load` put energy particles into pmap; the first tick's rebuild fixed that before,
but drawing a loaded state without a tick drew photons twice. Both maps are now built as TPT's
`RecalcFreeParticles` builds them.

**Streamlines** (`streams`). There are three rooms: stream cells over an oil fire, stream cells in still air
(including a diagonal pair, where TPT ends a line), and stream cells over burning gas whose lines cross patterned
walls.
- From each stream cell, TPT traces the air's velocity for up to 1024 steps, adding white each step, then draws the
  stream's icon from its font.
- It does this cell by cell among the other walls, so a line under a later wall's pattern is covered and one over
  an earlier wall's is not.
- The GPU traces each stream in its own thread. The resolve puts the pieces back into DrawWalls' cell order with
  the wall patterns (and blob display's blobs).

**Signs** (`signs`). The scene has 16 signs, TPT's limit:
- plain text in each justification;
- `{t}`, `{p}`, `{a}`, `{type}`, `{ctype}`, `{life}` and `{tmp}` over water, lava, burning oil and metal;
- the four kinds of link sign;
- a sign near the top (its box goes below), two overlapping;
- letters beyond ASCII and one the font lacks;
- BlendText's colour, underline and inversion codes.

How signs work here:
- Their text is formatted as `sign::getDisplayText` does, from what is at the sign's spot. The GPU writes that out
  each frame, and the host reads it the next time that frame slot comes round. The check reads it at once.
- Each sign is drawn over its box with TPT's font, which is exported from TPT into the render tables, in the
  signs' order.
- A save's signs come with it when it is pasted into a GPU world (`--check-save`: 16 of 16 the same). A GPU world
  keeps up to 256 signs where TPT keeps 16.

## Newtonian gravity (Phase 4)

TPT's Newtonian gravity (`N`) treats some elements as masses: NBHL and NWHL add and take mass as their tmp says,
GRVT, GPMP and GBMB set it. Every cell then pulls with −M_GRAV·mass·d/|d|³ summed over all cells, which TPT
computes as an FFT convolution on a thread while the next frame runs. Here that is `shaders/gravity.slang`:
- the mass gathered during a tick is transformed, multiplied by the kernel's spectrum and transformed back;
- a tick uses the field from two ticks' mass before, as TPT's exchange with its thread does;
- cells boxed in by GRAV walls (TPT's mask, a flood fill from the world's edge) neither pull nor feel a pull;
- TPT pads its grid to twice the world; here it is padded to powers of two of at least 2·cells − 1, which gives the
  same sums.

Everything that reads the field does so as TPT does:
- movement, scaled by each element's NewtonianGravity;
- liquids spreading across the field rather than sideways;
- pressure transitions under strong gravity (`gravtot`);
- DEUT's compression and O2's fusion;
- stick men;
- the warp display's lensing.

The field display (ctrl+G) and the gravity zones (shown with the GRAV wall tool) are drawn as TPT draws them.

**Measured** (`gravity` with `--newtonian`, 600 ticks, checkpoints every 100). The scene has three rooms:
- NBHL in a cloud of dust and gas, with a GRAV-walled box of dust;
- a GBMB falling onto sand;
- NWHL over water.

| Run | TPT seed | TPT order | GPU (7×7) |
|---|---|---|---|
| seed 1 | 8.6% | 9.0% | 8.5% |
| seed 2 | 10.4% | 9.7% | 7.7% |
| seed 1, serial | 8.6% | 9.0% | 9.8% |

- **The field** matches TPT's to float rounding at every checkpoint: the RMS difference is under 0.05%, from the
  field check `--compare` now prints with `--newtonian`.
- **The outcome** is the same as TPT's. The black hole eats dust and gas and heats up until the gas catches fire.
  The bomb leaves craters in the sand. The water dips under the white hole. High gravity turns gas into oil (seed
  2: 530 OIL on the GPU; TPT's three runs made 25–507, fewer when more of the gas burned first).
- **Drawing:** `--render-check --newtonian` with the field display, the zones, or both matches TPT exactly
  wherever TPT doesn't draw at random. The only differing pixels are the bomb's random flare.
- **Speed:** at 4K the FFT costs about 0.76 ms a tick with Newtonian gravity on. With it off, particles skip the
  field (it is zero then, as in TPT).

**Fixed along the way: a driver miscompile.** With Newtonian gravity added, nothing fell on the GPU any more, in
any scene, whether Newtonian gravity was on or off.
- The update kernel's `if (!solid && (gravity || newtonianGravity)) gravityField(...)` gave a pull of 0 for every
  element, on NVIDIA's driver 595.91.07.
- The SPIR-V (and Slang's GLSL) computed it correctly. A particle's values recorded on the GPU showed the right
  inputs and the right result from the same call made outside the `if`, yet 0 from the call inside it.
- The pull is now computed for every element and dropped for solids, which gives the same result.
- Every update-kernel change is now followed by a quick check that particles still move, before longer runs.

## Saving (Phase 5)

**World files** (`.bpw`, `--check-world`). Each scene runs 100 ticks, is saved, and is loaded into a fresh GPU world.
- **Kept exactly:** every particle field under its id, the air, walls and fans, Newtonian gravity's maps, stick
  men, signs, the settings, the tick and the seed.
- **Continuing:** both worlds then run 50 ticks more, one particle at a time so the runs repeat. They stay
  identical in every scene tried: mixed, stickmen, signs, pistons, gravity, circuit, portals, electric, emp, soap,
  pipes, tron.
  - That needs the engine's own state as well, beyond what TPT's saves keep: what is inside portals, wifi channels
    and the other counters, electrified walls. Without it, `portals` diverged.
  - `life` can't be checked this way. Two copies of the same file, run alongside, already differ: new LIFE get
    their ids from a free list built in parallel, and the Game of Life's neighbour lists follow id order.

| World | Particles | File | Save | Load |
|---|---|---|---|---|
| 4K `mixed` | 3.0M | 10.9 MB | 0.39 s | 0.40 s |
| 4K `water` (full) | 8.2M | 2.1 MB | 0.48 s | 0.79 s |
| 7680×4320 `mixed` | 11.8M | 21.8 MB | 1.1 s | 1.5 s |
| 7680×4320 `water` (full) | 33M | 3.7 MB | 1.7 s | 3.6 s |

Particles are stored field by field (all the x's, then all the y's…), which is what makes them compress so well.
Saving got 10× faster once read-backs from the GPU went through host-cached memory.

**TPT saves of an area** (stamps, `--check-export`). A 612×384 area of a running GPU world is made into a save by
the port of `Simulation::Save` (sim/GpuSave, as a stamp is), written by TPT's own `GameSave`, then loaded with TPT's
own loader.
- The particles, walls, signs and SOAP links TPT gets are exactly what was in the area, after TPT's own save
  encoding: whole pixels, temperatures and velocities at the precision OPS stores them, life up to 65535.
- That held in 15 scenes (12 small, 3 at 4K), including SOAP, stick men, portals, pipes and signs.
- What TPT's own saves change, these change too: OPS keeps only SOAP's forward links (loading rebuilds the back
  links from them), and loading hands fighters new slots.

## Sleeping chunks (Phase 5): measured, not built

Sleeping chunks would skip areas where nothing happens. To stay as accurate as everything else, a chunk could only
sleep while skipping it changes nothing. `--sleep-stats` measures how much of a scene that could ever be: after 600
ticks, the share of particles in 64×64 chunks where nothing changed bit for bit (particles in and within 2 px of
the chunk, its air) over the next tick.

| Scene (4K) | Particles | In unchanged chunks after +1 tick | After +10 ticks |
|---|---|---|---|
| pile (settled dust) | 651k | 2.4% | 2.3% |
| sink | 3.9M | 0% | 0% |
| walls | 1.6M | 0% | 0% |
| mixed | 2.6M | 0% | 0% |
| layers | 4.1M | 0% | 0% |

Why TPT's particles never quite stop:
- velocities are recomputed every tick;
- heat sharing between equal temperatures moves them by rounding;
- liquids keep trying random sideways moves;
- the air keeps shifting.

Exact sleeping would save almost nothing, so it isn't built.

## Tools (Phase 6)

TPT's tools change the world one call at a time: CreateParts for each point of the brush, CreateLine stamping the
brush along a line, CreateBox, FloodParts, CreateWalls and the rest (simulation/Editing.cpp). Classic mode runs that
code itself. The GPU engine reaches the same result in three steps:
- **On the CPU,** sim/Editing follows the same calls and counts how often each pixel gets CreatePartFlags. TPT's
  line points come from a copy of its loop built with TPT's compiler settings.
- **On the GPU,** spawn.slang applies CreatePartFlags to each of those pixels that many times: replace mode,
  specific delete, create_part from the brush (drawing onto CLNE and the other CtypeDraw elements, sparking,
  walls that block) and delete_part.
  - Two calls are all that can differ from one: the first removes an energy particle, the second what is under it.
  - TPT's order between pixels only decides which ids new particles get.
- **Walls** go through TPT's wall functions on the CPU's copy of the walls, in TPT's order. Streamlines depend on
  what was drawn just before, and so do the fans' velocities.
- **The simulation tools** (HEAT, COOL, AIR, VAC, AMBP, AMBM, PGRV, NGRV, WIND, CYCL, MIX) and **the property tool**
  work the same way: sim/Editing counts where TPT's SimTool and PropertyTool reach, and sim_tool.slang applies them
  there (sim/Editing.cpp, shaders/sim_tool.slang).
  - Most act on the air cell or the particle at each pixel the same way every time. They are applied as many times as
    TPT applies them, one after another, so TPT's float rounding and clamping come out the same.
  - CYCL's push depends on where each cell's corner is from the brush's centre. GCC builds TPT's `1/sqrtf` there as an
    estimate plus a refinement step, so the GPU can't recompute it. The amounts come from a copy of TPT's code built
    with TPT's flags, as a table over the brush.
  - The property tool parses values with TPT's AccessProperty rules (element and Game of Life names, `0x`, `#` and
    `0b` numbers, temperatures with K, C or F) and changes types with part_change_type's rules.
  - MIX swaps random particles in TPT too. The swaps are drawn in advance in TPT's order and made one after another, so
    they follow the same rules (same state, within reach), but which particles swap differs.
- **The decoration tools.**
  - SET, CLR, ADD, SUB, MUL and DIV change each channel of a particle's colour from its own old value only. So a copy
    of TPT's ApplyDecoration built with TPT's flags tabulates what each of the 256 values becomes, and sim_tool.slang
    looks it up at each pixel as often as TPT applies it.
  - SMUDGE averages the colours around each pixel as TPT's loop has left them, in an sRGB, linear or gamma colour
    space with `pow`. It runs on the CPU, in TPT's order, over colours read back from the GPU.
  - A decoration fill compares the picture as last drawn (TPT's renderer frame) and sets the colour over what it
    reaches.
- **Custom Game of Life types** live in the renderer's table buffer, so LIFE made with their rules (by the brush,
  CLNE or anything else) takes their colours, as TPT's LIFE Create looks them up.

**Checked** with `--compare --check-tools`. Each of 83 cases starts TPT and the GPU from the same world: particles,
photons over glass, PUMP and GPMP, walls of several kinds, a fan and signs. Each case then uses the same tools, and the
two results are compared: particles cell by cell (with their velocities and flags), the air maps (pressure, velocity,
ambient heat) and Newtonian gravity's mass. Every case comes out identical:

| What | Cases |
|---|---|
| Brushes | circle, square, triangle, ellipse; at the world's edge |
| Strokes | presses, drags (radius 0 too), shift lines, a whole stroke, ctrl boxes |
| Flood fills | inside a ring, the whole open area, gas past a gas-only wall, TESC |
| Erasing | over photons (once, and twice by dragging); erase-fills of METL and PHOT; an erase-fill that started on a wall erases the wall |
| Replace mode | anything, one element, photons, glass under photons (which TPT leaves), glass with nothing |
| Specific delete | one element, anything |
| Special tools | sparks on METL, INST and CLNE; LIGH, whose second bolt waits for a tick; TESC by brush, box and fill; Game of Life rulesets, on CLNE too; STKM with its spawn point; two fighters |
| Walls | wall presses, drags, boxes, fills, erase-fills; ERASEALL (particles and signs); streamlines next to one another; a fan's velocity, then a fan press; GRAV |
| Heat tools | HEAT and COOL by press, drag and box, at 0.1, 1 and 10 times (shift and ctrl); on PUMP and GPMP; under photons; down to 0 K |
| Air tools | AIR and VAC by press, drag and box, up to the pressure limit; AMBP and AMBM with ambient heat on and off; PGRV and NGRV |
| Wind and cyclones | WIND dragged, as a line and as Tool::Drag (each frame of a shift line), and pressed (nothing); CYCL by press, drag (round and square brushes) and box |
| Property tool | temp (in C and K), ctype (an element, a Game of Life rule), type (to GLAS, to PHOT by flood fill, to NONE), life by flood fill, dcolour and tmp on photons, vx, flags |
| Decoration | SET by press, drag (half alpha, radius 0) and box, on photons; CLR, ADD, SUB, MUL, DIV over two set colours (ADD onto none too); SMUDGE across two colours in each of the four colour spaces, by drag, press and box, and by the edge (where TPT doesn't); SET and CLR fills of the picture |
| Custom Game of Life | LIFE of a custom type (its colours), and of a rule no type has |
| Button signs | their create_part(-1) spark on METL, and on INST (one particle, not the whole INST) |

MIX is checked by what it does, on stripes of SAND and DUST beside water. TPT changed 920 cells and the GPU 936; neither
made or lost a particle, and every swap was between two particles of the same state.

The same check also drives GameView's port (ui/GameInput) with scripted presses, drags, frames and keys. Each
script makes the tool calls TPT's GameView.cpp makes. `--compare --check-screen` drives the whole game screen
(app/GameScreen) the same way, without a window, in Classic mode and on the GPU. It covers element search (E), the
property tool (ctrl+P) and its window, the sample tool (with shift, the property window with the particle's value), and
HEAT held down. It also covers the deco menu (B, TPT's first colour, a colour preset, the colour picker typed into, the
sample tool taking the picture's colour, B back), and a custom Game of Life type made in its window, drawn with, and
removed with ctrl+shift+right click and Confirm. Signs too: a new one, one edited (its text changing as it is typed),
moved and deleted, and a button sign on METL clicked with another tool (it sparks, and nothing is drawn).

**Speed** at 4K (the GPU's work included):

| Use | Time |
|---|---|
| A press, radius 20 | 2 ms |
| A drag of 200 px, radius 20 | 1 ms |
| A line across the world with the biggest brush (radius 200) | 30 ms |
| A flood fill of the empty world (8.2M particles) | 29 ms |
| Erasing them with a flood fill | 27 ms |
| A box of walls, 1000 × 1000 | 1 ms |
| HEAT along a line across the world, radius 200 | 47 ms |
| AIR along the same line | 31 ms |
| CYCL along the same line | 3 ms |
| A CYCL drag of 200 px, radius 20 | 1 ms |
| A MIX drag of 200 px, radius 50 | 13 ms |
| The property tool's flood fill (temp) | 3 ms |
| A SET drag of 200 px, radius 20 | 1 ms |
| ADD along a line across the world, radius 200 | 35 ms |
| A SMUDGE drag of 200 px, radius 20 | 79 ms |
| A SET fill of the whole picture | 78 ms |

A flood fill waits for the GPU, because it reads back what it checks. These were measured while another program was
using the GPU, so they may be a little slow.

## Undo, copy, paste and stamps (Phase 6)

**Undo** (`--check-undo`, `--check-screen`) follows TPT's history: a snapshot of the world before each press, paste,
cut, clear and reload, five of them kept, ctrl+Z back one, and ctrl+Y forward again as far as where ctrl+Z started.
- Classic mode takes TPT's own snapshots (`Simulation::CreateSnapshot` and `Restore`).
- A GPU world copies everything that carries over from tick to tick into host-cached memory in one submission,
  then compresses it on a thread while the game goes on. A restore brings back exactly what was copied.
- TPT keeps all but the newest as differences from the next; these are whole compressed snapshots, which behave
  the same and take more memory.

| 4K world | Particles | Pause | Compressed after | Kept | Restore |
|---|---|---|---|---|---|
| `mixed` | 3.0M | 59 ms | 61 ms more | 31 MB (345 MB raw) | 0.30 s |
| `sand` | 3.4M | 59 ms | 135 ms more | 77 MB (376 MB raw) | 0.38 s |
| `empty` | 0 | 59 ms | 19 ms more | 0.2 MB | 0.14 s |

**Copy, cut and paste** (`--check-clipboard`) go through a plain form of TPT's `GameSave` (classic/TptSave) with
none of its limits, so a GPU world can copy and paste areas bigger than TPT's world. Classic mode runs TPT's own
`Save`, `Load` and `clear_area`; the GPU worlds run their ports (sim/GpuSave). `Load` decides one particle at a time
what it places, what it removes and which stick man slots it takes; those decisions are made on the CPU in the same
order, and the GPU carries them out. The check builds the same 612×384 world in both (walls, air with pressure,
signs, stick men, linked SOAP, PIPE, QRTZ's pressure, INVIS under FILT) and compares:
- Copies of an area, with and without the air, and one past the world's corner: the saves are identical.
- The port of `GameSave::Transform` against TPT's own: four rotations, both mirrors, and rotated and mirrored, each
  nudged by 0, (1, 2) and (3, 3) pixels, and the whole world rotated: identical. The port cuts a save to the
  world's size where TPT cuts it to its own.
- Seven pastes, in the open, over water, over glass with photons, past each corner, rotated and mirrored, and three
  more in a row: the worlds are identical, every particle field, the walls, fans, air and signs. SOAP's links (ids
  that differ between the engines) are compared by the cells they join.
- A cut: identical.

TPT's quirks are kept:
- A pasted STKM's Create makes its spawn point where it lands.
- A pasted FIGH takes two fighter slots, its ChangeType's and then `Load`'s own, and the first stays taken.
- A SOAP link to something not pasted is left as it was.
- Pasted particles go on top of their cell, except that INVIS and FILT only take an empty one (`RecalcFreeParticles`).

Where a GPU world differs:
- With Newtonian gravity on, only the field comes with a paste; the mass and the mask are worked out again the next
  tick.
- The particles' part of `ApproximateBlockAirMaps` (which uses TPT's random numbers) is left to the next tick's
  recompute, as is everything blocking air in TPT a tick later.
- Elements the GPU doesn't run yet are left out, and the paste says which.

TPT saves opened in a GPU world are pasted the same way (`--check-save`). On saves of the `soap` scene after 120
ticks, `pipes` and `stacking`, every particle lands in its cell, all 220 SOAP links join the same cells and the air is
identical. Running the stacking save on, the piles collapse at different ticks than in TPT: TPT picks them at random
(`rng.between(0, 1599) <= count + 100`, one tick in ten).

**The preview** of what is being placed is TPT's `SaveRenderer` (the world's display preset) drawn at half opacity
with its dotted rectangle, through the camera. One bigger than TPT's world is drawn in 612×384 pieces, each with a
32-pixel margin for what spreads past a particle. Zoomed out, each screen pixel averages the preview's pixels it
covers. While an area is being chosen the world darkens outside it, as TPT darkens its own.

**Stamps** are TPT's: files named after the time, in an order kept in `stamps.json` (most recently used first). A
stamp TPT can read is a TPT `.stm`; a bigger one is in our own format, which keeps every field exactly. The browser
(`K`) is TPT's, with pictures as `ThumbnailRendererTask` makes them (TPT's default renderer, no fire, its lanczos
resampling). It makes one picture a frame rather than on a thread, and deletes at once without TPT's progress window.

## Settings, display options and the rest of the keys (Phase 6)

**The display** is TPT's `RendererSettings` kept as the game's state: the render, display and colour modes, the heat
display's range, the decoration level, the grid (`G`) and the element being found (Ctrl+`F`). The GPU's renderer
draws the grid and the finding as TPT's does. `--render-check --grid 3 --find WATR` on `plants` matches TPT's
renderer pixel for pixel in all 13 presets (the grid and the dimming change 29% of TPT's pixels, so the match
covers them).
- Not checked against TPT: finding by a property's value (the property tool's ctrl+F), and the grid's
  checkerboard (only a pref turns it on).

**The renderer options window** (RenderView) shows the world drawn live under it, not ticking. Its checkboxes and
preset buttons set the modes; `--check-screen` checks that a preset key there loads the preset and changes the
picture.

**The heat display's range** (TPT's `[TEMP L:... H:...]` in the HUD) is read back from the GPU's `hdisp` pass with the
cursor's sample. It shows only when that frame's scan saw something, as TPT's `hdispLimitValid` does.

**The sand effect** (`P` quick option; TPT's `pretty_powder`): powders made with it take a tint around
`sandcolour_interface × 1.3` (from a tool) or `sandcolour` (during a tick), plus a random alpha. Those are random
in TPT too, so `--check-tools` checks the spread. STNE drawn with the tint at 12 comes out with every channel in
15 ± 35 on both, means within 0.5 of each other and of 15, and alpha means of 75.2 and 74.4 (TPT's
`between(0, 149)`).
- Not checked: the tick's `sandcolour` path (powders that elements create while they run).

**The settings window** (OptionsView) sets each setting as it is changed, with TPT's ranges. Typed values that are
out of range aren't applied. An empty box takes the default once it loses focus. Left out are TPT's online options
(avatars, the server) and the console's.

The framerate caps work differently from TPT's:
- **"Uncapped" simulation:** runs 8 ticks for every frame drawn.
- **Rendering cap:** defaults to following the display. Big Powder aims at 30–240 fps on big screens, where TPT's
  default is 60.

**Prefs** are kept in `~/.local/share/big-powder/powder.pref`, in TPT's format (nested JSON under the same keys),
read when the game starts. Each is saved when TPT saves it:
- the settings window's as they change;
- the display, the HUD's debug mode, Newtonian gravity, ambient heat, the sand effect and the decoration colour as
  the game quits;
- favourites, custom Game of Life types, the property tool's value and the GOL window's last name and rule as they
  change.

What TPT keeps per save (air mode, gravity mode, heat, water equalisation) isn't saved. The command line's settings
override the prefs. `--check-screen` restarts the game screen on the prefs its run left and checks that the
favourite, the ambient air temperature and the display came back.

**The rest of TPT's keys** go through GameView::OnKeyPress's order:
- `F` steps a frame. `H` hides the HUD, `D` or F3 shows the debug HUD, F1 (or Ctrl+`H`) the introduction.
- `=` resets the air, Ctrl+`=` the sparks, `I` inverts the air, Ctrl+`U` resets the ambient heat.
- `P` or F2 takes a screenshot: the renderer's picture of the whole world as a PNG, named as TPT names it.
- The display presets: `0`–`9`, Shift+`6`, Shift+`0`, and Shift+`1` with the debug HUD.

Closing the window quits at once with fast quit on. With it off, closing the window closes the window on top
first, as TPT's `Engine::CloseWindow` does. `--check-screen` checks these keys, both windows, favourites, the
screenshot, the prefs and quitting: 72 of 72.

## Online saves (Phase 7)

The save browser and preview are TPT's SearchView, SaveButton and PreviewView over the same server requests
(SearchSavesRequest, SearchTagsRequest, GetSaveRequest, GetCommentsRequest, ThumbnailRequest,
GetSaveDataRequest), read only.
- `--check-online` asks TPT's real servers for the front page, the newest saves, a text search and the popular
  tags. Then, for five saves, their details, comments, thumbnail and data, each read with TPT's own loader. All of
  it answered as expected on 2026-11-14.
- Those five saves (9 to 79 KB, 22,390 to 88,895 particles), pasted into a GPU world with `--check-save`: every
  particle in the same cell as TPT's loader puts it, the signs the same, the air identical.
- `--check-screen` drives the browser with a made-up source. It covers the front page with its tags, typing a
  search (sent once typing pauses, as TPT does), a save's preview with its details, data and comments, and Open:
  Classic opens it in place of the world, a GPU world gets it to place.
- Not checked: the whole flow in a real window against the real server. Each half is checked on its own.

## Known limits

- **Liquids.** The GPU engine remains slightly behind TPT at levelling out, and a little foamier.
- **Newtonian gravity in huge worlds.** The FFT grid grows with the world: 1024×1024 for 4K, 4096×4096 (about
  670 MB, roughly 16 times the 4K cost) for 7680×4320. Phase 5's huge worlds will need a cheaper scheme.
- **Particle order.** TPT updates in id order; after loading a save that is screen order, top to bottom. No
  parallel scheme reproduces that exactly. The target is to be as close to TPT as TPT is to itself under a
  different order.

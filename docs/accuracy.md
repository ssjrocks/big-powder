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

## Known limits

- **Liquids.** The GPU engine remains slightly behind TPT at levelling out, and a little foamier.
- **Particle order.** TPT updates in id order; after loading a save that is screen order, top to bottom. No
  parallel scheme reproduces that exactly. The target is to be as close to TPT as TPT is to itself under a
  different order.

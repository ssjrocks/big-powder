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

## Known limits

- **Liquids.** The GPU engine remains slightly behind TPT at levelling out, and a little foamier.
- **Particle order.** TPT updates in id order; after loading a save that is screen order, top to bottom. No
  parallel scheme reproduces that exactly. The target is to be as close to TPT as TPT is to itself under a
  different order.

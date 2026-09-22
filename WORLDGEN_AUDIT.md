# Stages 6 and 7B source audit

Audited against the local Minecraft 26.3 Java decompilation under
`mc-26.3-source/src/main/java/net/minecraft/`, using the diagram as the stage
checklist. The supported target is **new, normal Overworld terrain**.

## Corrections

- **Material biome lookup:** added `BiomeManager`'s eight-corner jittered quart
  selection and SHA-256 seed obfuscation. Queries can cross chunk boundaries;
  surface queries clamp the palette's Y range, while carver repair can resolve
  out-of-range quart Y through the biome source.
- **Icebergs:** added the frozen temperature modifier's fixed-seed 2-D simplex
  stack and the two-block melting adjustment, including the minimum-size check.
  Deep frozen oceans always exceed the melting threshold at sea level.
- **Steep surfaces:** use `gradientX <= -4 || gradientZ >= 4`, as specified by
  `SteepCondition`, rather than absolute gradient magnitudes.
- **Live surface heights:** material extensions, air replacements, and carving
  update the world-surface height used by subsequent gradients and soil repair.
- **Carver bounds:** use the new-world mask bounds `[-63, 312]`, with the
  ellipsoid loop excluding its lower bound. Geometry can therefore mark
  `[-62, 312]`, protecting the lowest two and highest seven block layers.
- **Carver application:** visit X/Z columns and ascending vertical runs, visiting
  each run top-down. Reset exposed-grass tracking between runs.
- **Heightmaps:** powder snow contributes to world surface, but not ocean floor
  or either motion-blocking heightmap, matching the 26.3 block tags.
- **Rule boundaries:** preserve inclusive surface-noise conditions, double
  thresholds where Java uses doubles, sulfur's first-match priority at `0.4F`,
  and float ore-richness map endpoints/evaluation order.
- **Aquifers:** use the source's empty-fluid sentinel, floodedness threshold
  mapping order, and double lava-noise threshold.
- **Biome registration:** preserve the source's valley entry order, including
  overlapping temperature/continentalness/erosion boundaries.

These are intentional terrain-output changes. The 7A implementation and its
recorded density values are unchanged.

## Source coverage

| Area | Java references | Result |
| --- | --- | --- |
| Climate quantization and intervals | `world/level/biome/Climate.java`, `OverworldBiomeBuilder.java` | Float-to-long quantization, standard interval table, surface/depth/cave entries checked. All standard offsets are zero. Tie policy differs as described below. |
| Palettes and block-biome queries | `BiomeManager.java`, `NoiseBasedChunkGenerator.java` | Quart palettes plus jittered block queries, boundary resolution, and seed obfuscation implemented. |
| Base fill | `NoiseBasedChunkGenerator.java`, `NoiseChunk.java` | Integer Z/X/descending-Y fill, default stone, aquifer substance, fluid-update marking, and pass ordering checked. |
| Aquifers | `Aquifer.java`, `NoiseRouterData.overworldAquifers`, `NoiseData.java` | Noise settings, nearest-center ordering, exclusion, surface probes, fluid levels/types, pressure and scheduling branches reviewed. The chunk-wide high-altitude sampling shortcut is an optimization absent from this scalar sampler. |
| Materials | `data/worldgen/material/OverworldMaterialRules.java`, `MaterialSystem.java`, `MaterialRuleContext.java`, condition classes, `OreVeinRule.java` | Bedrock → copper → iron → surface → sulfur/deepslate priority; column context, bands, extensions, and top-material repair reviewed. |
| Temperature | `Biome.java`, `data/worldgen/biome/OverworldBiomes.java`, `synth/SimplexNoise.java`, `GradientNoise.java`, `NoiseStack.java` | Sea-level frozen-ocean melting uses fixed seeds, discarded-offset RNG consumption, float accumulation and double coordinates. |
| Carvers | `data/worldgen/Carvers.java`, `carver/WorldCarver.java`, `CaveWorldCarver.java`, `CanyonWorldCarver.java`, `chunk/CarvingMask.java` | Standard three-carver list, source seeds, providers, geometry, bounds and mask application checked. All supported standard Overworld biomes use this list. |
| Heightmaps | `Heightmap.java`, `data/tags/VanillaBlockTagsProvider.java` | First-free Y and supported block-tag predicates checked. Bedrock is the only supported `UNCARVABLE` block. |

The 3×3 possible-biome collection and material-rule compilation in Java prune
rule evaluation; this implementation evaluates its built-in rules directly.
Neighbor biome queries are resolved through the stable biome provider without
requiring neighboring terrain generation. Density buffer pools and compiled
samplers are represented by this library's scalar samplers and caches.

## Reference tests

The reference calculations were run in temporary Java source-launcher harnesses
using methods extracted from the local source and small adapters for the required
types. No decompiled source is included in the repository. These are component
oracles, **not a vanilla server chunk-generation comparison**.

`tests/terrain_tests.cpp` contains:

- **9,100 surface climate cases:** the Java builder emits 7,594 parameter points;
  a brute-force first-registered search is compared across every temperature,
  humidity, erosion and weirdness slice at four inland distances. Separate
  tests cover ocean, underground, quantization, and boundary cases.
- **Five full carving masks:** FNV-1a hashes and bit counts from Java geometry,
  vanilla providers, Java random, and lookup-table trigonometry. Seeds include
  zero, positive and negative values, with positive and negative chunk positions.
- **Six zoom-seed and selection fixtures:** Java SHA-256 byte ordering and full
  selection-grid hashes, including signed-long endpoints and negative coordinates.
- **52,052 iceberg-temperature probes:** the Java fixed-seed simplex/temperature
  calculation yields 28,044 melting positions and a recorded full-grid hash.
- **Behavioral regressions:** directional slopes, live heights during soil repair,
  vertical-run order, neighbor-biome queries, powder-snow heightmaps, vein ranges,
  fluid updates, and generation-order determinism.

The numeric fixtures do not require Java or the decompilation to run. Build and
run them with the headless commands in `README.md`.

## Remaining compatibility boundaries

The graph's supported new-world stages are substantially covered, but this is
still **not a seed-compatible or bit-identical Minecraft implementation**:

1. World-seeded density noise and aquifer/material positional factories retain
   the project's existing seed convention. Consequently, full chunks differ
   from Minecraft for the same numeric seed.
2. Biome nearest-point ties use deterministic first-registration order. Java's
   R-tree traversal and cached previous result can select another equally close
   entry. The interval-table oracle deliberately tests this project's tie policy.
3. Carver trigonometry and simplex math depend on the host math library. The
   component fixtures establish agreement for their tested inputs, not a proof
   for every seed, coordinate, compiler and platform.
4. Saved-world biome/height/density/carving blending and below-zero retrogen,
   structure-reference resolution, other dimensions/generators, and custom
   data-pack compilation are outside the current API. Density blend/beardifier
   inputs remain injectable. Stage 8 decoration and subsequent stages are also
   outside this terrain implementation.
5. The block palette represents terrain materials rather than the full block
   state registry. Fluid post-processing is queued, not simulated.

An end-to-end parity claim would require replacing the existing seed/noise
convention, resolving biome traversal semantics, and comparing full Java and C++
terrain outputs—including palettes, heightmaps and fluid-update positions.

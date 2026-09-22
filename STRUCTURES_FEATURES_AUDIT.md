# Stages 5 and 8 source audit

## Verdict

**Stages 5 and 8 are still not complete Minecraft implementations.** The
follow-up below replaces several missing execution components and feature
algorithms. Vanilla content coverage, assets, state, and side effects remain
incomplete. The original audit started with four procedural structure families
and nine hard-coded feature entries; its findings are retained below as history.

This audit compared the stage-5/8 implementation (`src/structures.cpp`,
`src/decoration.cpp`, `src/generation.cpp`), its public API, and its terrain
integration with the local 26.3 decompilation. Java paths below are relative to
`mc-26.3-source/src/main/java/net/minecraft/`. The generated Java source is not
part of the committed deliverable. The earlier biome/terrain audit remains in
[WORLDGEN_AUDIT.md](WORLDGEN_AUDIT.md).

## Implemented after the initial audit

### Structures

- Weighted selection without replacement, retrying failed variants with the
  Java large-feature legacy random stream. Singleton sets consume no selection
  draw. The four existing families now retain their village/mineshaft/portal/
  ancient-city variant identities and use variant-specific biome eligibility.
- Public `StructureTemplateCatalog` input, wired through `GenerationOptions`.
  Pool assembly rotates templates and connector orientations, matches connector
  names/targets, handles rollable joints, respects selection/placement priorities,
  tries weighted shuffled candidates and fallback pools, and tracks occupied
  space, internal connectors, depth, and distance limits.
- Explicit template block placement, connector final blocks, ground deltas,
  rigid/terrain-matching projection, reciprocal jigsaw junctions, and positional
  material ignore/rule/rot processors. Template catalogs fail validation on
  unknown IDs or invalid sizes/weights rather than silently substituting boxes.
- Beardifier collection now filters nearby pieces, excludes terrain-matching
  pool boxes, and adds eligible junctions at Java's `0.4F` weight. Non-pool pieces
  use ground delta zero. Junction boundary tests use strict source inequalities.

The catalog API accepts decoded material-level data, **not NBT files**. The local
source checkout contains Java code but no structure NBT resource tree. No vanilla
template assets or complete pool catalog have been added. Without a configured
catalog, structure geometry remains the prior procedural approximation. The
template engine does not yet implement pool aliases, expansion hacks, liquid
settings, all processors, multiple palettes, block-state rotation, template
entities, structure-specific `afterPlace`, or the complete jigsaw start options.
Terrain-matching blocks use the supplied preliminary surface-height sampler,
rather than vanilla's final terrain heightmap. These are remaining stage-5/8
gaps, not a claim that the template engine is fully vanilla-compatible.

### Features

- `feature_placement.cpp`: identity-based, ordered reverse-DFS FeatureSorter with
  dependency-cycle rejection and global per-step indices. The decoration loop
  selects those indices from the actual 3×3 biome union.
- Depth-first modifier execution: count, count-extra, rarity, square, heightmap,
  uniform/triangular/very-biased height providers, biome and block predicates,
  offsets, and environment scans. Nested sequence/selector features retain the
  same RNG and top-level biome context. Success never short-circuits remaining
  terminal placements.
- `features.cpp`: source-based ore ellipsoids, containment pruning, once-per-block
  testing, ordered target rules, and air-exposure discard. Standard coal, iron,
  gold, redstone, diamond, lapis, copper, emerald, and underground rock/dirt/gravel
  variants use the source counts, sizes, rarity, height distributions, and
  discard probabilities. Stone/deepslate ore materials are distinct.
- Disk column geometry and state-provider support, including sand-to-sandstone
  support rules; configured water and lava springs; fixed-seed, height-adjusted
  biome temperatures and frozen-ocean modifiers for surface freezing/snow.

The per-biome catalog still includes only supported entries; it is not the full
vanilla biome-generation registry, so global feature indices and therefore
seeds still differ. Tree shapes, detailed snow/block states, biome disk/tree
membership, many feature types (vegetation, lakes, dungeons, cave decoration,
etc.), and generation side effects are still incomplete. New ore materials use
fallback atlas tiles in the viewer until their textures are added.

### Template catalog example

This small authored marker illustrates the API, not a vanilla village asset:

```cpp
#include <mcworld/structure_templates.hpp>

auto catalog = std::make_shared<mcworld::StructureTemplateCatalog>();
mcworld::StructureTemplate marker;
marker.size = {1, 1, 1};
marker.blocks = {{{0, 0, 0}, mcworld::Block::Bricks}};
catalog->templates["marker"] = marker;
catalog->pools["markers"].elements = {{"marker", 1, mcworld::PieceProjection::Rigid}};
catalog->starts[mcworld::StructureVariant::VillagePlains] = {"markers", 0, 16, 120, false};
mcworld::GenerationOptions options;
options.templates = catalog;
mcworld::OverworldNoiseRouter router(12345);
mcworld::OverworldWorldGenerator generator(router, options);
```

Keep the catalog immutable for the generator's lifetime. Positions in template
definitions are local; positions in generated piece block lists are world-space.

### Added verification

The generation suite now also tests reverse-DFS ordering/cycles, depth-first RNG
consumption, nested features and biome filters, weighted retries, template catalog
validation, deterministic rotated assembly, empty-element termination, reciprocal
junction heights, projection-aware beard density, processor chaining, public
catalog-to-decoration integration, and live template-write heightmaps.

Four independent Java source-expression ore fixtures check complete voxel-mask
hashes, block counts, and the next RNG value after placement, including a size-64
vein and air-exposure discard. They are component oracles, not full-world parity.

Follow-up verification passed: all 3 headless tests, all 8 viewer-enabled tests
(including headless rendering and CLI validation), and `generation_tests` under
AddressSanitizer/UndefinedBehaviorSanitizer. The standalone density and terrain
characterization tests continue to pass without updated expectations.

## Original audit corrections

- **Structure biome tags:** villages now allow only plains, meadow, desert,
  savanna, snowy plains, and taiga. Mineshafts allow oceans and reject deep dark.
  Ruined portals allow dripstone/lush/sulfur caves and reject deep dark. These
  are the unions of the supported families' Overworld tags; variant-specific
  generation is still absent.
- **Public start bounds:** `StructureStart::bounds()` now includes the
  twelve-block inflation for terrain-adapting structures, matching Java's
  `getBoundingBox()`. Reference collection uses these bounds without inflating
  twice. Piece bounds remain unadjusted.
- **Beard arithmetic:** squared distances preserve X/Y/Z addition order and the
  kernel uses the source's power-of-e expression. Java-derived raw-float fixtures
  cover ordinary samples, large ground offsets, and kernel boundaries.
- **Decoration biome lookup:** placement-time queries now use the existing
  SHA-256-seeded, jittered block-biome lookup over neighboring quart palettes.
  Previously they used the containing quart cell directly.
- **Placement-time filtering:** clay disks and water springs now check their
  origin biome after nearby-biome feature selection. A neighboring biome may
  select a feature, but does not authorize all its origins. The membership
  table itself remains a reduced catalog, not vanilla biome feature data.
- **Water springs:** require configured rock above and below, allow either air
  or configured rock at the origin, and count exactly four rocks and one hole
  across the four horizontal neighbors plus below. The old generic six-neighbor
  motion-blocking count incorrectly accepted ceiling holes and arbitrary solid
  blocks. Valid materials are the Java water-spring list intersected with the
  available palette; `Snow` represents the existing snow material abstraction.
- **Snow/freezing traversal:** uses `MOTION_BLOCKING`, rather than
  `WORLD_SURFACE`, and X-then-Z traversal. The temperature and snow-survival
  approximation still needs replacement, as described below.
- **Structure write bounds:** the writable chunk box excludes the lowest world
  layer, as in `ChunkGenerator.getWritableArea()`.

These intentionally change stage-5/8 output. Standalone 7A and 7B algorithms and
recorded density values were not edited.

## Original diagram/source findings (before the follow-up above)

| Diagram operation | Java reference | Implementation and remaining gap |
| --- | --- | --- |
| Structure enable flag | `world/level/chunk/ChunkGenerator.java` | Supported independently of the feature flag. Turning off features still allows structure metadata and adaptation. |
| Placement candidates | `data/worldgen/StructureSets.java`, `world/level/levelgen/WorldgenRandom.java` | Village, portal, and ancient-city random-spread constants and Java legacy positional seeds; mineshaft large-feature `nextDouble` frequency gate. No complete structure-set registry, concentric rings, or general exclusion/frequency policies. |
| Weighted selection and retries | `ChunkGenerator.createStructures()` | Missing. Each family is collapsed to one entry, with no weighted variant retry list. |
| Generation points, allowed biomes, piece lists | `data/tags/BiomeTagsProvider.java`, `world/level/levelgen/structure/Structure.java` | Family tag unions corrected, but generation points use approximate surface/fixed heights. Boxes and shells replace actual structure algorithms, NBT templates, jigsaw pools, processors, rotations, and terrain validation. Mineshaft piece-level deep-dark blocking is not implemented. |
| Store starts and notify | `structure/StructureStart.java`, `ChunkGenerator.java` | In-memory starts/pieces exist. Saved-start reuse, reference counts for locating, persistence, and `StructureCheck` notification are absent. |
| Radius-8 reference scan | `ChunkGenerator.createReferences()`, `Structure.adjustBoundingBox()` | Valid starts intersect adjusted target footprints; references carry structure kind and source chunk. These are vector entries with unique family/source construction, not Java registry-keyed sets. |
| Beardifier before aquifers/materials | `world/level/levelgen/Beardifier.java` | Referenced adjusting boxes feed the stage-7B beardifier seam. Bury/thin/box/encapsulate formulas exist. Pool-element rigid versus terrain-matching projection, jigsaw junction contributions, and Java's per-piece collection/affected-box pruning are missing. Current procedural pieces have neither projection nor junction data. |
| Mutable 3×3 feature region | `ChunkGenerator.applyBiomeDecoration()` | A two-chunk terrain halo supplies the one-chunk decoration-source/write halo. Writes update blocks and all four heightmaps; fluid work items are cleaned and deduplicated. This is a synchronous area policy, not Java's chunk-status scheduler. |
| FeatureSorter | `world/level/biome/FeatureSorter.java` | Missing dependency graph and cycle detection. A fixed array supplies local per-step indices. These are not vanilla registry-derived feature indices. |
| Decoration and feature seeds | `WorldgenRandom.java`, `ChunkGenerator.applyBiomeDecoration()` | Xoroshiro-backed bit-draw wrapper, chunk-minimum block coordinates, and `decorationSeed + index + 10000 * step` follow the source. Reduced catalogs and different random consumption prevent matching actual vanilla feature streams. |
| Eleven steps, structures first | `ChunkGenerator.applyBiomeDecoration()`, `structure/StructureStart.java` | Ordinal loop, per-stage structure index, feature index, and stored piece order exist. Generic shell placement replaces `postProcess`; there is no `afterPlace` hook or complete per-stage structure registry. |
| Nearby-biome union and feature selection | `ChunkGenerator.applyBiomeDecoration()` | 3×3 palette union, deduplication through membership flags, ascending catalog indices, and placement-time block-biome checks. No complete biome-generation settings or possible-biome registry filtering. |
| Depth-first modifiers and nested features | Diagram `FeaturePlacer` branch | No generic implementation. Count/position/height choices are embedded in feature functions. Full count/range distributions, modifier order, scans, predicates, nested features, and shared-stream consumption remain to be ported. |
| Feature geometry | `world/level/levelgen/feature/`, `data/worldgen/features/` | Ores use random walks rather than vanilla vein geometry/discard rules; clay and oak-like trees are simplified; springs now match the supported placement predicate but retain approximate count/height distribution. Most configured features and tree species are absent. |
| Freeze/snow | `feature/SnowAndFreezeFeature.java` | Corrected traversal/heightmap and zoomed biome query. Still uses a hard-coded snowy-biome predicate, generic support test, and one snow material. Missing altitude/temperature modifiers, light checks, exact snow support rules, and snowy block states. |
| Write side effects | Diagram `WorldGenRegion.setBlock` branch | Blocks, heightmaps, and deferred fluid positions only. POI, block entities, loot, shape updates, scheduled ticks, entities, and full block states are absent. |

## Original verification

`tests/generation_tests.cpp` now checks:

- Allowed and rejected village biomes, ocean mineshafts, cave portals, and
  deep-dark exclusions at already-eligible placement coordinates.
- Adjusted versus raw structure bounds, including neighboring chunk footprints.
- Java-derived beard-kernel float bits and the asymmetric `[-12, 12)` limits.
- Spring rock tags, air origins, ceiling/floor holes, and exact hole count.
- A referenced village changing terrain before any structure blocks are placed.
- Disabled structures/features preserving standalone stage-7B blocks.
- Existing ore/tree output, live heightmaps, valid fluid work items, and bounds
  validation.

The beard fixtures were evaluated with a temporary Java source-launcher harness
using the source's numeric expressions. This is a component check, not a server
chunk oracle. The dependency-free build and all three headless CTest entries
passed. No full vanilla chunk comparison was performed.

## Work required for a full parity claim

1. Complete the structure registry, placement policies, actual family generation
   algorithms, vanilla template/pool assets, remaining jigsaw/processor semantics,
   and structure-specific hooks.
2. Complete the biome feature lists, remaining modifiers/configured features,
   and required block states/side effects on top of the new execution machinery.
3. Replace the existing noise/keyed-RNG and biome tie-breaking compatibility
   differences in stages 6/7 with vanilla behavior; otherwise equal seeds still
   select different terrain and biomes even with exact stage-5/8 algorithms.
4. Compare structure metadata and final blocks across seeds and chunk borders
   against a real 26.3 generation oracle. Deterministic generation for a fixed
   requested area does not establish equivalence across different area shapes
   or Java scheduling orders: neighboring features observe earlier writes.

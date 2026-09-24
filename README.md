# Minecraft 26.3 Overworld worldgen

A C++20 graph-level implementation of Overworld structure starts/references (5),
biome lookup (6), density and terrain generation (7A/7B), and decoration (8) in
`minecraft-26.3-worldgen.dot`. This repository provides the `mcworld` library
and a headless `terrain_gen` CLI. Export generated blocks as `.schem` files and
render them with [voxel-viewer](https://github.com/nuxdie/voxel-viewer), located
in `../voxel-viewer` in the local workspace.

This is a graph-level implementation, not a bit-for-bit or seed-compatible Java port.
It keeps the Minecraft 26.3 graph, constants, terrain splines, cave branches,
slides, interpolation, biome climate intervals, aquifer pressure calculations,
material-rule ordering, and cave/canyon geometry while using a stable C++ noise
seed derivation and keyed positional randomness. Noise sampling includes seeded coordinate
offsets, parity octave normalization, coordinate wrapping, and vertical smearing
for blended terrain noise. The same numeric seed will produce different terrain
in Minecraft.

## Build

Requires CMake 3.24+ and a C++20 compiler with thread support. The default build
has no graphics dependencies or downloads. Python 3 enables importer and CLI tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

All builds are headless. Set `-DMCWORLD_BUILD_TESTS=OFF` to omit tests.

### Import Minecraft structure assets

With a local **Minecraft 26.3 game JAR** (not a sources JAR), configure:

```sh
cmake -S . -B build -DMCWORLD_GAME_JAR="/path/to/minecraft-26.3.jar"
cmake --build build -j
```

This uses Python's standard library to import the five village variants and
ancient-city template/pool dependencies into `build/structures.mcwc`. The CLI
loads that catalog by default. Game assets stay in the ignored build directory.
The library build without `MCWORLD_GAME_JAR` retains its dependency-free path.

Alternatively, import a game JAR or extracted resources explicitly:

```sh
python3 tools/import_worldgen.py /path/to/minecraft-26.3.jar build/structures.mcwc
./build/terrain_gen --templates build/structures.mcwc --seed 12345 --export area.schem
```

A confirmed asset-backed village in this port's current terrain is at seed
`12345`, chunk `(-58, -53)`:

```sh
./build/terrain_gen --templates build/structures.mcwc --seed 12345 --chunk-x -58 --chunk-z -53 --chunks 3 --export village.schem
```

The importer records unsupported processors, feature-pool algorithms, template
entities, and missing resources in the catalog; the CLI prints their count.
This is asset-backed structure generation, **not complete vanilla parity**.

## Run

```sh
./build/terrain_gen --seed 12345 --chunk-x 0 --chunk-z 0 --chunks 4 --export area.schem
../voxel-viewer/build/voxel-viewer area.schem
```

The generator defaults to an 8×8 area around `--chunk-x` / `--chunk-z`.
Use `--chunks N` (1–16) for an N×N area, or `--chunks 1` for a single chunk.
Each axis runs from `center - N/2` (integer division) through `center - N/2 + N - 1`;
the default at (0, 0) covers chunks -4 through 3 on both axes (128×128 blocks).
Terrain generation uses up to eight workers by default. Use `--threads N`
(1–64) to tune it for the machine; `--threads 1` keeps generation serial.
Structure/feature finalization needs a two-chunk dependency halo beyond the
output area. Use `--terrain-only` for the faster independent stage-7B path,
without structures, decoration, or their dependency halo.

The CLI always prints phase timings and output-area `solid_blocks`,
`water_blocks`, and `lava_blocks` counts. Omit `--export` to generate statistics
without writing a file. Use `--help` for the complete CLI reference.

Build the sibling viewer separately following its README. It owns camera
controls, lighting, materials, and all meshing/render modes. For smooth rendering:

```sh
../voxel-viewer/build/voxel-viewer --smooth area.schem
```

The former `terrain_viewer` executable, `--voxel`/`--headless` flags,
`MCWORLD_BUILD_VIEWER`/`MCWORLD_FETCH_FAITHFUL_TEXTURES` build options, and
`mcworld/isosurface.hpp` mesh API have been removed. Use `terrain_gen` for
generation and voxel-viewer for presentation.

### Export to a schematic

`--export FILE.schem` saves the output area (the `--chunks` square, without the
generation halo) as a Sponge schematic, version 2. WorldEdit and
[voxel-viewer](https://github.com/nuxdie/voxel-viewer) open it directly:

```sh
./build/terrain_gen --seed 12345 --chunks 4 --export area.schem
../voxel-viewer/build/voxel-viewer area.schem
```

The Y range is trimmed to the lowest and highest non-air blocks, and the
`Offset` field records the world position of the schematic origin. Imported
block states from structure templates and plants (for example
`minecraft:oak_log[axis=x]`) are kept; other materials use their plain block ID.
Block entities, biomes and entities are not exported. The file is valid gzip,
but it uses stored (uncompressed) deflate blocks so the library needs no zlib,
which makes it larger than WorldEdit's own output.

## Generation coverage

- Java-seeded random-spread starts for graph-level villages, mineshafts, ruined
  portals, and ancient cities, with weighted biome-eligible variant retries;
  radius-8 references; adjusted piece bounds; and terrain-adaptation density.
  Optional template catalogs add rotated pool assembly, fallback pools, rigid
  and terrain-matching projections, processors, and jigsaw-junction density.
- Standard Overworld climate-interval lookup and 4×4×4 biome cells per section,
  including dappled forest, sulfur caves, and deep dark. Materials use Minecraft's
  SHA-256-seeded, jittered block-biome zoom, including neighboring quart cells.
- Density fill, enabled or disabled aquifers, pressure barriers, global water
  below Y=63 and lava below Y=-54, and fluid post-processing positions.
- Bedrock, copper/iron ore veins, biome surface and subsurface materials,
  badlands bands/pillars, temperature-adjusted frozen-ocean icebergs, and
  sulfur/deepslate rules.
- Cave, extra-underground cave, and canyon masks from the radius-8 source-chunk
  neighborhood, followed by aquifer-aware carving and exposed-soil repair.
- Final world-surface, ocean-floor, and motion-blocking heightmaps.
- The eleven decoration stages in ordinal order, with referenced structure
  pieces before dependency-sorted biome features. Placement modifiers execute
  depth-first, including nested features on the same random stream. Ores use
  the source ellipsoid geometry, standard distributions, deepslate variants,
  and air-exposure rules. Disks, water/lava springs, oak/birch/spruce/pine trees,
  forked acacia trees and cactus/berry/grass/flower patches in village pools,
  village block piles, and
  temperature-adjusted freezing/snow write through a mutable 3×3 region and
  maintain all four heightmaps.

## Terrain library API

```cpp
#include <mcworld/terrain.hpp>

mcworld::OverworldNoiseRouter router(12345);
mcworld::OverworldTerrainGenerator generator(router);
auto chunk = generator.generate(-1, 0);
auto block = chunk.at(8, 64, 8);       // local X/Z, world Y
auto biome = chunk.biomeAt(8, 64, 8); // quart-resolution palette
auto firstFreeY = chunk.worldSurface[8 * 16 + 8];
```

`TerrainOptions` can disable aquifers, ore veins, or carvers and accepts a
`BiomeSource` override. The router must outlive the generator; use a separate
pair per thread and keep injected inputs stable. Density blending and beardifier
inputs are supplied through the router. Each chunk owns its blocks and palettes;
heightmaps store first-free Y, with -64 for empty columns. Fluid-update positions
are chunk-local X/Z and world Y, deduplicated after carving. They are work items
for a later simulation stage, not simulated fluid flow.

`TerrainChunk::at`, `set`, and `biomeAt` check bounds. Generator coordinates
reserve room for the carver neighborhood, aquifer probes, and sampling halos;
out-of-range chunk requests throw `std::invalid_argument`.

For finalized stage-5-through-8 chunks, use the area generator so all feature
sources in the one-chunk write halo run before chunks are returned:

```cpp
#include <mcworld/generation.hpp>

mcworld::OverworldNoiseRouter router(12345);
mcworld::OverworldWorldGenerator generator(router);
auto area = generator.generateArea(-4, -4, 8, 8);
const auto& generated = area.at(0, 0);
auto block = generated.terrain.at(8, 64, 8);
auto starts = generated.starts;
auto references = generated.references;
```

`writeSpongeSchematic` from `<mcworld/schematic.hpp>` saves a generated area
(or any contiguous row-major list of `TerrainChunk`s) as a `.schem` file:

```cpp
#include <mcworld/schematic.hpp>

std::ofstream file("area.schem", std::ios::binary);
mcworld::writeSpongeSchematic(file, area);
```

`GenerationOptions` independently controls structure metadata/adaptation and
the FEATURES pass. `OverworldTerrainGenerator::generate()` remains the stable,
independent stage-7B API; its beardifier overload is the integration seam used
by `OverworldWorldGenerator`.

`GenerationOptions::templates` accepts an immutable `StructureTemplateCatalog`
from `<mcworld/structure_templates.hpp>`. It supplies explicit template blocks,
connectors, weighted pools, fallback IDs, processors, and start pools keyed by
`StructureVariant`. Catalogs are validated when constructing the generator.
Configured variants use assembled templates instead of procedural boxes; missing
template IDs in a configured pool are errors. `loadStructureTemplateCatalog(path)`
loads the binary catalog generated by `tools/import_worldgen.py`. Minecraft's
NBT assets are imported from the supplied JAR rather than bundled with this repo.
See the [stage-5/8 implementation notes](STRUCTURES_FEATURES_AUDIT.md).

### Compatibility boundaries

This remains a graph-level implementation, not a vanilla parity claim:

- Biome selection uses the standard interval table with first-registered
  tie-breaking rather than Java's stateful R-tree traversal. The block-biome
  zoom algorithm and seed obfuscation follow Java, but operate on this port's
  climate-derived palettes.
- Positional random streams for aquifers/materials use the project's keyed seed
  convention. Carvers use the Java 48-bit LCG and large-feature seeding, with
  lookup-table trigonometry constructed using the host math library.
- Frozen-ocean temperature adjustment uses Java's fixed-seed simplex noises;
  iceberg geometry still uses the project's world-seeded noise convention.
- Block IDs describe generated base materials, not the complete block-state
  registry. Imported state names/properties and block-entity NBT payloads (including
  loot-table references) are retained in `TerrainChunk::blockData`, accessible
  through `dataAt()`. Facing/axis/rotation and directional connection properties
   rotate with templates. Block-entity behavior, loot filling, and simulation are absent.
  Motion heightmaps follow the
  supported blocks' tags, including powder-snow and leaf exclusions.
- This generator covers new, normal Overworld chunks. Saved-world retrogen,
  carving blend filters, other dimensions, custom data-pack rule compilation,
  lighting, a chunk-status scheduler, and persistence are outside this API.
- Stage 5/8 execution now includes weighted variant selection, a template/pool
  engine, FeatureSorter, and a modifier executor, but content coverage is still
  incomplete. Unconfigured structures retain procedural boxes. The importer
  covers village and ancient-city assets, not all structure families. Full biome feature lists, every
  feature/modifier/processor type, and structure-specific hooks are not present.
  Pool aliases, liquid settings, and full block-state behavior also remain
  unsupported. The current catalog's feature indices differ from vanilla.
  Block-entity execution, loot filling, POI, entities, and scheduled ticks remain unimplemented.

See [the stages 6/7B source audit](WORLDGEN_AUDIT.md) for corrections, reference
test coverage, and the distinction between algorithm checks and full-world parity.
The [stages 5/8 source audit](STRUCTURES_FEATURES_AUDIT.md) maps structure and
decoration coverage to the diagram, records source-based fixes, and lists the
remaining implementation gaps. Stages 5/8 are not complete vanilla ports.

## Scope

The public API in `include/mcworld/worldgen.hpp` exposes temperature,
vegetation, continentalness, erosion, depth, ridges, chunk surface level, and
final density. Blending and structure beard density are injectable interfaces;
their defaults represent a newly generated world with no adapting structures.
Router instances cache samples and must be used by one thread at a time, with
stable blending/beardifier inputs. Invalid or out-of-range sampling coordinates
throw `std::invalid_argument`.

The library returns density samples and generated blocks. Schematic export is
the file boundary to external rendering; this repository contains no mesh
extraction, graphics backend, or texture assets.

## Working on the density graph

`src/` is organised around the 7A graph and 7B terrain stages:

| File | Role |
| --- | --- |
| `density_math.hpp` | Scalar primitives shared by every layer (`lerp`, `squeeze`, slides). |
| `legacy_random.hpp` | Java's 48-bit LCG and the positional seeds the terrain stages draw from. |
| `noise.cpp` | Perlin, `NormalNoise` octave stacks, and the base 3-D `BlendedNoise`. |
| `spline.cpp` | `TerrainProvider` cubic splines for offset, factor and jaggedness. |
| `worldgen.cpp` | The router itself: climate, sloped cheese, caves, slides, noodles. |
| `biome.cpp` | Standard Overworld climate intervals and nearest-point lookup. |
| `biome_environment.cpp` | Block-biome zoom and fixed-seed frozen-ocean temperatures. |
| `terrain_internal.hpp` | The contract between the 7B passes; not part of the public API. |
| `terrain.cpp` | Block storage, palettes, terrain pass ordering, and heightmaps. |
| `aquifer.cpp` | Fluid centers, levels, pressure barriers, and update decisions. |
| `materials.cpp` | Ordered bedrock, vein, surface, and underground rules. |
| `carvers.cpp` | Source-seeded cave/canyon masks and mask application. |
| `generation_internal.hpp` | The seam between the stage-5 and stage-8 halves; not part of the public API. |
| `structures.cpp` | Stage 5: structure placement, starts, references, and beardification. |
| `structure_templates.cpp` | Validated template catalogs, pool assembly, rotations, junctions, and material processors. |
| `template_catalog.cpp` | Binary imported-catalog reader and material fallbacks for retained game block states. |
| `decoration.cpp` | Stage 8: the 3x3 write region, structure pieces, and the feature catalog. |
| `feature_placement.cpp` | FeatureSorter, depth-first modifiers, integer providers, and nested feature execution. |
| `features.cpp` | Ore ellipsoids, disks/state providers, and spring algorithms. |
| `vegetation.cpp` | Straight/forked trunks, foliage algorithms, leaf distances, and village block piles. |
| `plant_features.cpp` | Village vegetation patches, cactus columns, and the fixed-seed plains-flower noise provider. |
| `generation.cpp` | Stage 5/8 pass orchestration, area planning, and the shared value types. |
| `schematic.cpp` | Dependency-free Sponge schematic export. |
| `main.cpp` | Headless generation/export CLI. |

Generation is float arithmetic, so it is sensitive in ways ordinary code is
not: re-associating a product, widening an intermediate to `double`, renaming a
noise key or changing a lattice spacing all silently produce different terrain.
Constants are named and commented rather than inlined so each one can be
audited against the Java source individually.

`testKnownDensities` in `tests/worldgen_tests.cpp` pins recorded outputs with a
small tolerance and catches ordinary breakage. It will not catch a change of a
few ULP. When refactoring this code for real, dump a large sample of
`sampleFinalDensity` and the full `RouterSample` as raw
float bit patterns before and after, and require the two dumps to be identical;
that is how the current structure was verified against its predecessor.

Stages 5 and 8 need the same procedure at a coarser grain, because their output
is blocks rather than floats: digest the whole `GeneratedArea` - blocks, biome
palette, all four heightmaps, the fluid-update queue, starts and references -
over several seeds and option combinations, and require the digests to match
byte for byte. That is how the `structures.cpp`/`decoration.cpp` split was
verified against the single file it replaced.

`tests/terrain_tests.cpp` covers biome boundaries, Java random vectors, aquifer
decisions, generation-order determinism, materials/vein height ranges, carving,
heightmaps, fluid-update validity, and public input validation. It also checks
Java-derived biome slice, full carving-mask, biome zoom, and iceberg-temperature
fixtures. The separate
`terrain_tests` executable can be selected with CTest's `-R '^terrain_tests$'`.

`tests/generation_tests.cpp` covers structure starts/references, disabled paths,
structure biome eligibility, Java beard-kernel fixtures, spring placement rules,
terrain adaptation, decoration output, and live feature heightmaps. It also tests
the feature dependency graph, depth-first/nested RNG consumption, Java-derived
ore masks, weighted variant retries, template pool assembly/placement, processors,
and jigsaw-junction density.

`tests/generation_cli_tests.py` checks CLI validation, terrain-only/finalized
exports, schematic contents, and deterministic exports across worker counts.

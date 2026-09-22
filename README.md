# Minecraft 26.3 Overworld terrain viewer

A C++20 graph-level implementation of Overworld structure starts/references (5),
biome lookup (6), density and terrain generation (7A/7B), and decoration (8) in
`minecraft-26.3-worldgen.dot`. The Raylib viewer displays an 8×8 chunk area as
either a smooth mesh of the generated terrain or generated voxel blocks.

This is a graph-level implementation, not a bit-for-bit or seed-compatible Java port.
It keeps the Minecraft 26.3 graph, constants, terrain splines, cave branches,
slides, interpolation, biome climate intervals, aquifer pressure calculations,
material-rule ordering, and cave/canyon geometry while using a stable C++ noise
seed derivation and keyed positional randomness. Noise sampling includes seeded coordinate
offsets, parity octave normalization, coordinate wrapping, and vertical smearing
for blended terrain noise. The same numeric seed will produce different terrain
in Minecraft.

## Build

CMake downloads the pinned Raylib 5.5 source and the hash-pinned Faithful 32x
26.3 resource pack when the viewer is enabled.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For a headless library and test build:

```sh
cmake -S . -B build-headless -DMCWORLD_BUILD_VIEWER=OFF
cmake --build build-headless -j
ctest --test-dir build-headless --output-on-failure
```

## Run

```sh
./build/terrain_viewer --seed 12345 --chunk-x 0 --chunk-z 0
```

The viewer generates a fixed 8×8 area around `--chunk-x` / `--chunk-z`.
Use `--chunks N` (1–16) for an N×N area, or `--chunks 1` for a single chunk.
Each axis runs from `center - N/2` (integer division) through `center - N/2 + N - 1`;
the default at (0, 0) covers chunks -4 through 3 on both axes (128×128 blocks).
The area is generated at startup and does not stream as the camera moves.

Use `--headless` to generate the complete mesh and print statistics without
opening a window. Combine it with `--voxel` to also generate and report voxel
statistics without opening a window.

Use `--voxel` to start in the block-style view:

```sh
./build/terrain_viewer --seed 12345 --chunk-x 0 --chunk-z 0 --voxel
```

Structure/feature finalization needs a two-chunk dependency halo beyond the
visible area. Use `--terrain-only` for the faster independent stage-7B path.

Controls:

- `WASD` and mouse: free camera at 40 blocks/second
- Hold `Shift`: boost flight speed to 160 blocks/second
- `Space` / left `Ctrl`: fly up / down
- `V`: switch between the smooth terrain mesh and voxel blocks
- `F`: toggle wireframe
- `Tab`: release or capture the cursor (camera input pauses while released)
- `Esc`: exit

Both views use the final generated terrain blocks. The smooth view runs marching
tetrahedra over the continuous density field. Final generated blocks supply a
3-D filtered density correction for carvers, aquifer barriers, and material
extensions. Only that correction is filtered, leaving unedited 7A density intact.
The reconstruction rounds rasterized carve boundaries rather than pinning every
crossing to a block edge; one-block details can be rounded off. Crossings use the
generated solid material. Separate water/lava surfaces preserve submerged ocean
floors and cave walls; the voxel view emits block faces. Generated neighboring chunks provide
consistent mesh samples and suppress internal voxel boundary faces. Both views
map the generated block material to Faithful textures; the smooth mesh uses
per-block planar projection over its curved triangles. Fluids are rendered as
opaque textured material; lighting is presentation-only. Headless
output reports `mesh_triangles` and, with `--voxel`, `solid_blocks`,
`water_blocks`, `lava_blocks`, and `voxel_faces`.

### Faithful 32x textures

Viewer builds download the Faithful 32x September 2026 release for Minecraft
26.3 from its [official Modrinth listing](https://modrinth.com/resourcepack/faithful-32x).
The archive is SHA-512 verified, stored under the build directory, and only the
44 block texture files used by the stage 7B terrain palette are extracted. It is
not committed to this repository. Configure with
`-DMCWORLD_FETCH_FAITHFUL_TEXTURES=OFF` to use generated fallback textures and
avoid this download after Raylib is available.

Textures are from **Faithful 32x** by the Faithful Resource Pack project:
[faithfulpack.net](https://faithfulpack.net). They are used under the
[Faithful License](https://faithfulpack.net/license). Faithful is not affiliated
with or endorsed by this project.

Generation includes:

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
  and air-exposure rules. Disks, water/lava springs, oak-like trees, and
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

`GenerationOptions` independently controls structure metadata/adaptation and
the FEATURES pass. `OverworldTerrainGenerator::generate()` remains the stable,
independent stage-7B API; its beardifier overload is the integration seam used
by `OverworldWorldGenerator`.

`GenerationOptions::templates` accepts an immutable `StructureTemplateCatalog`
from `<mcworld/structure_templates.hpp>`. It supplies explicit template blocks,
connectors, weighted pools, fallback IDs, processors, and start pools keyed by
`StructureVariant`. Catalogs are validated when constructing the generator.
Configured variants use assembled templates instead of procedural boxes; missing
template IDs in a configured pool are errors. Minecraft's NBT assets are not
bundled or automatically imported. See the [stage-5/8 implementation notes](STRUCTURES_FEATURES_AUDIT.md).

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
  registry. Fluids, snow, leaves, and structure materials have no orientation,
  simulation, loot, or block-entity state. Motion heightmaps follow the
  supported blocks' tags, including powder-snow and leaf exclusions.
- This generator covers new, normal Overworld chunks. Saved-world retrogen,
  carving blend filters, other dimensions, custom data-pack rule compilation,
  lighting, a chunk-status scheduler, and persistence are outside this API.
- Stage 5/8 execution now includes weighted variant selection, a template/pool
  engine, FeatureSorter, and a modifier executor, but content coverage is still
  incomplete. Unconfigured structures retain procedural boxes. Vanilla template
  NBT/pool catalogs, all structure families, full biome feature lists, every
  feature/modifier/processor type, and structure-specific hooks are not present.
  Pool aliases, expansion hacks, liquid settings, and full block states also
  remain unsupported. The current catalog's feature indices differ from vanilla.
  Block entities, loot, POI, entities, and scheduled ticks remain unimplemented.

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

The public 7A density extractor produces chunk-local X/Z positions and world Y.
It accepts Y bounds within `[-64, 320]` and samples a one-block halo for
consistent boundary normals. Its meshes are open at chunk boundaries. The
viewer instead meshes final 7B blocks and starts above the selected area's
highest generated surface, with framing scaled to the area size.

## Working on the density graph

`src/` is organised around the 7A graph and 7B terrain stages:

| File | Role |
| --- | --- |
| `density_math.hpp` | Scalar primitives shared by every layer (`lerp`, `squeeze`, slides). |
| `legacy_random.hpp` | Java's 48-bit LCG and the positional seeds the terrain stages draw from. |
| `noise.cpp` | Perlin, `NormalNoise` octave stacks, and the base 3-D `BlendedNoise`. |
| `spline.cpp` | `TerrainProvider` cubic splines for offset, factor and jaggedness. |
| `worldgen.cpp` | The router itself: climate, sloped cheese, caves, slides, noodles. |
| `isosurface.cpp` | Marching tetrahedra over the final density field. |
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
| `decoration.cpp` | Stage 8: the 3x3 write region, structure pieces, and the feature catalog. |
| `feature_placement.cpp` | FeatureSorter, depth-first modifiers, integer providers, and nested feature execution. |
| `features.cpp` | Ore ellipsoids, disks/state providers, and spring algorithms. |
| `generation.cpp` | Stage 5/8 pass orchestration, area planning, and the shared value types. |

Generation is float arithmetic, so it is sensitive in ways ordinary code is
not: re-associating a product, widening an intermediate to `double`, renaming a
noise key or changing a lattice spacing all silently produce different terrain.
Constants are named and commented rather than inlined so each one can be
audited against the Java source individually.

`testKnownDensities` in `tests/worldgen_tests.cpp` pins recorded outputs with a
small tolerance and catches ordinary breakage. It will not catch a change of a
few ULP. When refactoring this code for real, dump a large sample of
`sampleFinalDensity`, the full `RouterSample` and the extracted mesh as raw
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

The smooth and voxel views are two presentations of the same finalized stage
5-through-8 block result. The standalone `buildChunkIsosurface` API remains
available for inspecting the raw 7A density field.

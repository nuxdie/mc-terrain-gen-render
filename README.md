# Minecraft 26.3 Overworld density viewer

A C++20 implementation of step 7A in `minecraft-26.3-worldgen.dot`: the
standard Overworld `NoiseRouter` and final-density graph. The project samples
one chunk, extracts the `density = 0` surface, and displays it in an interactive
Raylib viewer.

This is a graph-level implementation, not a bit-for-bit or seed-compatible Java port.
It keeps the Minecraft 26.3 graph, constants, terrain splines, cave branches,
slides, interpolation, and named deterministic noise streams while using a
stable C++ noise seed derivation. Noise sampling includes seeded coordinate
offsets, parity octave normalization, coordinate wrapping, and vertical smearing
for blended terrain noise. The same numeric seed will produce different terrain
in Minecraft.

## Build

CMake downloads the pinned Raylib 5.5 source when the viewer is enabled.

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

Use `--headless` to generate the complete mesh and print statistics without
opening a window.

Controls:

- `WASD` and mouse: free camera
- `F`: toggle wireframe
- `Tab`: release or capture the cursor (camera input pauses while released)
- `Esc`: exit

## Scope

The public API in `include/mcworld/worldgen.hpp` exposes temperature,
vegetation, continentalness, erosion, depth, ridges, chunk surface level, and
final density. Blending and structure beard density are injectable interfaces;
their defaults represent a newly generated world with no adapting structures.
Router instances cache samples and must be used by one thread at a time, with
stable blending/beardifier inputs. Invalid or out-of-range sampling coordinates
throw `std::invalid_argument`.

Mesh positions are chunk-local in X/Z and use world Y. Extraction accepts Y
bounds within `[-64, 320]` and samples a one-block halo for consistent boundary
normals. Meshes are open at the chunk boundary; the viewer starts above the
selected chunk's highest surface.

## Working on the density graph

`src/` is organised to follow the 7A graph:

| File | Role |
| --- | --- |
| `density_math.hpp` | Scalar primitives shared by every layer (`lerp`, `squeeze`, slides). |
| `noise.cpp` | Perlin, `NormalNoise` octave stacks, and the base 3-D `BlendedNoise`. |
| `spline.cpp` | `TerrainProvider` cubic splines for offset, factor and jaggedness. |
| `worldgen.cpp` | The router itself: climate, sloped cheese, caves, slides, noodles. |
| `isosurface.cpp` | Marching tetrahedra over the final density field. |

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

The viewer intentionally stops at step 7A. It shows a smooth density
isosurface, not final Minecraft blocks. Aquifers, water and lava, surface
materials, ores, carvers, biome decoration, and lighting belong to later steps
in the diagram and are not represented.

# Repository Guide

## Build And Verify

- All builds are dependency-free and headless: `cmake -S . -B build-headless -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build-headless -j` and `ctest --test-dir build-headless --output-on-failure`.
- `terrain_gen` generates blocks and optionally exports `.schem` files. Rendering belongs to `../voxel-viewer`: `./build-headless/terrain_gen --seed 12345 --chunks 4 --export area.schem`, then `../voxel-viewer/build/voxel-viewer area.schem`. Python 3 enables the CLI/export and importer CTest entries.
- For a focused density build/test, use `cmake --build build-headless --target mcworld_tests -j` and `ctest --test-dir build-headless -R '^mcworld_tests$' --output-on-failure`. Terrain tests use the separate `terrain_tests` target/CTest entry. Logical cases within each executable cannot be filtered individually.

## Code Boundaries

- `mcworld` is the reusable C++20 library. Its public API is only under `include/mcworld/`; implementation-only noise, spline, and density graph code is under `src/`. `src/main.cpp` is the headless generation/export CLI, not a library entrypoint. Keep meshing, graphics dependencies, and rendering in the separate voxel-viewer project.
- The implementation covers graph-level Overworld stages 5 through 8: procedural structure starts/references and beardification, biome lookup, the 7A density graph, 7B terrain, and a bounded decoration catalog. Read README compatibility boundaries before claiming vanilla parity. Full template structures/features, lighting, retrogen, and other dimensions are not implemented.
- Schematic export preserves block-state names/properties and records the world origin; output excludes generation dependency halos.
- `mc-26.3-source/`, when present, is an ignored, generated decompilation used only to audit the port. It requires Java 25 to regenerate and must not be committed or redistributed.

## Terrain Invariants

- Preserve the deliberate `double` coordinate / `float` density split, expression evaluation order, noise resource keys, octave data, lattice spacing, and cache fill order. Reassociation, widening intermediates, or renaming keys changes generated terrain.
- `tests/worldgen_tests.cpp::testKnownDensities` is a characterization guard with `1e-6` tolerance, not proof of bit-identical output. For density-graph refactors, compare raw float bit patterns for many `sampleFinalDensity` values and full `RouterSample`s before and after.
- If generation changes intentionally, update the recorded density/router values and make the behavior change explicit; do not silently loosen their tolerance.
- `OverworldNoiseRouter` mutates caches from its `const` sampling methods. Use one router per thread, and keep injected `BlendSampler` and `Beardifier` behavior stable for that router's lifetime.
- Terrain generation samples integer block positions in Z/X/descending-Y order. Blocks and fluid-update positions use local X/Z and world Y; heightmaps store first-free Y. Preserve world-coordinate positional seeds and source-chunk carver seeds so generating neighbors in different orders cannot change blocks.
- The terrain generator references its router, which must outlive it. Biome providers must remain stable.

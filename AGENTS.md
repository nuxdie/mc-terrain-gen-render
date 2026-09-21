# Repository Guide

## Build And Verify

- Prefer the dependency-free headless build for library work: `cmake -S . -B build-headless -DMCWORLD_BUILD_VIEWER=OFF`, then `cmake --build build-headless -j` and `ctest --test-dir build-headless --output-on-failure`.
- A viewer-enabled configure downloads the pinned Raylib 5.5 source. Its CTest suite also exercises `terrain_viewer --headless` and invalid CLI input: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`.
- For a focused build/test, use `cmake --build build-headless --target mcworld_tests -j` and `ctest --test-dir build-headless -R '^mcworld_tests$' --output-on-failure`. All logical unit cases live in one custom executable and cannot be filtered individually.

## Code Boundaries

- `mcworld` is the reusable C++20 library. Its public API is only under `include/mcworld/`; implementation-only noise, spline, density graph, and meshing code is under `src/`. `src/main.cpp` is the Raylib viewer, not a library entrypoint.
- The implementation covers step 7A of `minecraft-26.3-worldgen.dot`: the Overworld density graph and a smooth zero-density isosurface, not final blocks, aquifers, materials, carvers, decoration, or lighting.
- Isosurface vertices use chunk-local X/Z but world Y. Extraction samples a one-block halo for normals and intentionally leaves chunk boundaries open.
- `mc-26.3-source/`, when present, is an ignored, generated decompilation used only to audit the port. It requires Java 25 to regenerate and must not be committed or redistributed.

## Terrain Invariants

- Preserve the deliberate `double` coordinate / `float` density split, expression evaluation order, noise resource keys, octave data, lattice spacing, and cache fill order. Reassociation, widening intermediates, or renaming keys changes generated terrain.
- `tests/worldgen_tests.cpp::testKnownDensities` is a characterization guard with `1e-6` tolerance, not proof of bit-identical output. For density-graph refactors, compare raw float bit patterns for many `sampleFinalDensity` values, full `RouterSample`s, and extracted meshes before and after.
- If generation changes intentionally, update the recorded density/router values and make the behavior change explicit; do not silently loosen their tolerance.
- `OverworldNoiseRouter` mutates caches from its `const` sampling methods. Use one router per thread, and keep injected `BlendSampler` and `Beardifier` behavior stable for that router's lifetime.

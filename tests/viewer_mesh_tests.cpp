#include "voxel_renderer.hpp"
#include "terrain_surface_field.hpp"
#include "mcworld/structure_templates.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <tuple>

int main() {
    mcworld::OverworldNoiseRouter router(12345);
    viewer::VoxelTerrain terrain(router);
    const auto left = terrain.buildSmoothMesh(-1, 2);
    const auto right = terrain.buildSmoothMesh(0, 2);
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; ++failures; }
    };
    // A rasterized cylindrical 7B tunnel cut through constant positive 7A
    // density. Measure geometric error, not just whether triangles exist.
    const auto correction = [](int x, int y, int) {
        const bool solid = std::hypot(x - 0.2F, y - 0.35F) >= 6.3F;
        return viewer::detail::terrainCorrection(0.4F, solid);
    };
    float binaryError = 0, smoothError = 0;
    int binaryCrossings = 0, smoothCrossings = 0;
    for (int y = -8; y <= 8; ++y) for (int x = -8; x < 8; ++x) {
        for (bool smooth : {false, true}) {
            const auto field = [&](int px) {
                return 0.4F + (smooth ? viewer::detail::filteredCorrection(px, y, 0, correction)
                                      : correction(px, y, 0));
            };
            const float a = field(x), b = field(x + 1);
            if ((a > 0) == (b > 0)) continue;
            const float crossing = x + a / (a - b);
            const float error = std::abs(std::hypot(crossing - 0.2F, y - 0.35F) - 6.3F);
            (smooth ? smoothError : binaryError) += error;
            ++(smooth ? smoothCrossings : binaryCrossings);
        }
    }
    check(smoothCrossings > 0 && binaryCrossings > 0 &&
          smoothError / smoothCrossings < binaryError / binaryCrossings,
          "Reconstructed carver is closer to a curved tunnel than binary voxel edges");
    check(0.4F + viewer::detail::filteredCorrection(0, 0, 0, correction) < 0,
          "Reconstruction preserves the tunnel interior");
    check(viewer::detail::filteredCorrection(20, 20, 0, correction) == 0,
          "Unedited 7A density is preserved exactly");
    const auto thinRoof = [](int, int y, int) {
        return viewer::detail::terrainCorrection(0.4F, y == 0);
    };
    check(0.4F + viewer::detail::filteredCorrection(0, 0, 0, thinRoof) > 0 &&
          0.4F + viewer::detail::filteredCorrection(0, 1, 0, thinRoof) < 0,
          "Reconstruction preserves a one-block cave roof and adjacent air");
    bool continuous = false, water = false;
    using Seam = std::tuple<float, float, float, float, float, int, int, int>;
    std::set<Seam> a, b;
    for (const auto* mesh : {&left, &right}) {
        check(!mesh->vertices.empty() && mesh->vertices.size() % 3 == 0, "Complete terrain triangles");
        for (const auto& v : mesh->vertices) {
            check(std::isfinite(v.x + v.y + v.z + v.nx + v.ny + v.nz + v.u + v.v),
                  "Finite geometry, normals and texture coordinates");
            check(v.u >= 0.0F && v.u <= 1.0F && v.v >= 0.0F && v.v <= 1.0F,
                  "Smooth texture coordinates stay inside the atlas");
            continuous |= std::abs(v.y * 2 - std::round(v.y * 2)) > 0.01F;
            water |= v.blue > v.red * 2 && v.blue > v.green;
            if (v.x == (mesh == &left ? 16.0F : 0.0F)) {
                (mesh == &left ? a : b).emplace(v.y, v.z, v.nx, v.ny, v.nz, v.red, v.green, v.blue);
            }
        }
    }
    check(continuous, "Solid crossings use continuous density, not binary midpoints");
    check(water, "Water has a separate colored surface");
    check(!a.empty() && a == b, "Adjacent chunks agree on seam positions, normals and materials");

    const auto voxel = terrain.buildMesh(-1, 2);
    std::set<std::pair<float, float>> voxelTiles;
    for (const auto& v : voxel.vertices) {
        check(std::isfinite(v.u + v.v) && v.u >= 0.0F && v.u <= 1.0F && v.v >= 0.0F && v.v <= 1.0F,
              "Voxel texture coordinates stay inside the atlas");
        // Each face starts at the same local UV corner, independent of atlas size.
        if ((&v - voxel.vertices.data()) % 6 == 0) {
            voxelTiles.emplace(v.u, v.v);
        }
    }
    check(voxelTiles.size() > 1, "Voxel materials select multiple atlas textures");

    mcworld::OverworldTerrainGenerator generator(router);
    const auto blocks = generator.generate(-1, 2);
    int underwaterColumns = 0, meshedColumns = 0;
    for (int z = 1; z < 15; ++z) for (int x = 1; x < 15; ++x) {
        const int floor = blocks.oceanFloor[z * 16 + x] - 1;
        if (floor < -64 || floor >= 319 || blocks.at(x, floor + 1, z) != mcworld::Block::Water) continue;
        ++underwaterColumns;
        bool found = false;
        for (const auto& v : left.vertices) {
            const bool fluidColor = (v.blue > v.red * 2 && v.blue > v.green) ||
                                    (v.red > v.green * 2 && v.blue < 20);
            if (!fluidColor && std::abs(v.x - x) < 1 && std::abs(v.z - z) < 1 &&
                v.y >= floor - 1 && v.y <= floor + 2) { found = true; break; }
        }
        meshedColumns += found;
    }
    check(underwaterColumns > 0, "Fixture contains an ocean floor");
    check(meshedColumns == underwaterColumns, "Submerged rock surfaces remain in the mesh");
    int ceilings = 0;
    for (int z = 1; z < 15; ++z) for (int x = 1; x < 15; ++x) {
        for (int y = -63; y < 100; ++y) {
            if (blocks.at(x, y, z) != mcworld::Block::Air ||
                !mcworld::isSolid(blocks.at(x, y + 1, z))) continue;
            ++ceilings;
            bool found = false;
            for (const auto& v : left.vertices) {
                // Reconstruction has one block of support: ceiling crossings
                // can move off the original lattice edge, but must stay local.
                if (std::abs(v.x - x) <= 1 && std::abs(v.z - z) <= 1 &&
                    v.y > y - 1 && v.y < y + 2) { found = true; break; }
            }
            check(found, "Cave ceiling has a surface crossing");
        }
    }
    check(ceilings > 0, "Fixture exercises cave ceilings");

    // Two imported vegetation blocks isolated above the terrain exercise the
    // actual voxel path, including non-occluding plants and inset cactus sides.
    class Plains final : public mcworld::BiomeSource {
        mcworld::Biome sample(const mcworld::OverworldNoiseRouter&, int, int, int) const override { return mcworld::Biome::Plains; }
    };
    auto catalog = std::make_shared<mcworld::StructureTemplateCatalog>();
    mcworld::StructureTemplate marker;
    marker.size = {1, 2, 1};
    marker.blocks = {{{0,0,0}, mcworld::Block::Cactus}, {{0,1,0}, mcworld::Block::Plant}};
    catalog->templates["vegetation"] = marker;
    catalog->pools["vegetation"].elements = {{"vegetation", 1, mcworld::PieceProjection::Rigid}};
    catalog->starts[mcworld::StructureVariant::VillagePlains] = {"vegetation", 0, 16, 279, false};
    mcworld::GenerationOptions options;
    options.templates = catalog;
    options.terrain.biomes = std::make_shared<Plains>();
    options.terrain.carvers = options.terrain.aquifers = options.terrain.oreVeins = false;
    viewer::VoxelTerrain vegetation(router, options);
    vegetation.prepareArea(21, 5, 1, 1);
    const auto vegetationMesh = vegetation.buildMesh(21, 5);
    int plantVertices = 0, cactusTopVertices = 0, cactusSideVertices = 0;
    for (const auto& v : vegetationMesh.vertices) {
        // Exclude integer-edged cubes made by the village beardifier around
        // the marker. Both vegetation shapes have strictly inset X/Z vertices.
        if (v.x <= 0 || v.x >= 1 || v.z <= 0 || v.z >= 1 || v.y < 278 || v.y > 280) continue;
        check(std::isfinite(v.u + v.v + v.nx + v.nz), "Vegetation quads have finite UVs and normals");
        if (std::abs(v.nx) > .7F && std::abs(v.nz) > .7F) {
            ++plantVertices;
            check(v.y >= 279 && v.y < 280, "Plants use crossed quads rather than full cubes");
        } else if (v.ny > .9F && v.y == 279) ++cactusTopVertices;
        else if (std::abs(v.nx) > .9F || std::abs(v.nz) > .9F) {
            ++cactusSideVertices;
            check(v.x >= .0625F && v.x <= .9375F && v.z >= .0625F && v.z <= .9375F, "Cactus side geometry is inset by one pixel");
        }
    }
    check(plantVertices == 24 && cactusTopVertices == 6 && cactusSideVertices == 24,
          "Plant does not hide supporting cactus top; crossed quads render on both sides");
    for (std::size_t i = 0; i + 2 < vegetationMesh.vertices.size(); i += 3) {
        const auto& a = vegetationMesh.vertices[i];
        if (std::abs(a.nx) < .7F || std::abs(a.nz) < .7F) continue;
        const auto& b = vegetationMesh.vertices[i + 1];
        const auto& c = vegetationMesh.vertices[i + 2];
        const float nx = (b.y-a.y)*(c.z-a.z) - (b.z-a.z)*(c.y-a.y);
        const float nz = (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x);
        check(nx * a.nx + nz * a.nz > 0, "Crossed plant face normals agree with triangle winding on both sides");
    }
    return failures ? 1 : 0;
}

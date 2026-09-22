// Interactive viewer for generated Minecraft 26.3 Overworld terrain. It runs
// stages 5 through 8, then presents the final blocks as a smooth surface mesh
// or exposed voxel faces.

#include "mcworld/worldgen.hpp"
#include "mcworld/structure_templates.hpp"
#include "voxel_renderer.hpp"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::int64_t seed{0};
    int chunkX{0};
    int chunkZ{0};
    int chunks{8};
    bool headless{false};
    bool voxel{false};
    bool terrainOnly{false};
#ifdef MCWORLD_DEFAULT_TEMPLATE_CATALOG
    std::string templates{MCWORLD_DEFAULT_TEMPLATE_CATALOG};
#else
    std::string templates;
#endif
};

// --- Command line ----------------------------------------------------------

template <typename Number>
Number parseNumber(std::string_view text, std::string_view option) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("Invalid value for " + std::string(option) + ": " + std::string(text));
    }
    return value;
}

void printUsage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [--seed N] [--chunk-x N] [--chunk-z N] [--chunks N] [--voxel] [--terrain-only] [--headless]\n"
        << "\n"
        << "Renders Minecraft 26.3 standard Overworld terrain as a smooth mesh or voxel blocks.\n"
        << "--chunks N: N x N area around the selected chunk (default 8, range 1..16).\n"
        << "--voxel: start in voxel mode; with --headless, also build the voxel mesh.\n"
        << "--terrain-only: skip structure and feature finalization.\n"
        << "--templates FILE: load a generated Minecraft structure catalog (.mcwc).\n"
        << "WASD + mouse: fly; Space/Ctrl: up/down; Shift: 4x speed.\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (argument == "--headless") {
            options.headless = true;
            continue;
        }
        if (argument == "--voxel") {
            options.voxel = true;
            continue;
        }
        if (argument == "--terrain-only") {
            options.terrainOnly = true;
            continue;
        }
        if (i + 1 >= argc) {
            throw std::invalid_argument("Missing value after " + std::string(argument));
        }
        const std::string_view value = argv[++i];
        if (argument == "--seed") {
            options.seed = parseNumber<std::int64_t>(value, argument);
        } else if (argument == "--chunk-x") {
            options.chunkX = parseNumber<int>(value, argument);
        } else if (argument == "--chunk-z") {
            options.chunkZ = parseNumber<int>(value, argument);
        } else if (argument == "--chunks") {
            options.chunks = parseNumber<int>(value, argument);
        } else if (argument == "--templates") {
            options.templates = value;
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
        }
    }
    if (options.chunks < 1 || options.chunks > 16) {
        throw std::invalid_argument("--chunks must be between 1 and 16");
    }
    for (const int center : {options.chunkX, options.chunkZ}) {
        const std::int64_t first = static_cast<std::int64_t>(center) - options.chunks / 2;
        const std::int64_t last = first + options.chunks - 1;
        // Both meshers sample generated neighboring chunks. Terrain generation
        // itself reserves 512 blocks for carvers, aquifers and sampling halos.
        if ((first - 1) * 16 < static_cast<std::int64_t>(std::numeric_limits<int>::min()) + 512
            || (last + 1) * 16 > static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 512) {
            throw std::invalid_argument("Chunk area is outside the supported integer grid");
        }
    }
    return options;
}

// Keep display coordinates near the selected chunk, even at large world positions.
template <typename Mesh, typename Build>
Mesh buildArea(const Options& options, Build build) {
    Mesh area;
    for (int z = 0; z < options.chunks; ++z) {
        for (int x = 0; x < options.chunks; ++x) {
            const int dx = x - options.chunks / 2;
            const int dz = z - options.chunks / 2;
            Mesh chunk = build(options.chunkX + dx, options.chunkZ + dz);
            for (auto& vertex : chunk.vertices) {
                vertex.x += static_cast<float>(dx * 16);
                vertex.z += static_cast<float>(dz * 16);
            }
            area.vertices.insert(area.vertices.end(), chunk.vertices.begin(), chunk.vertices.end());
            if constexpr (requires { area.solidBlockCount; }) {
                area.solidBlockCount += chunk.solidBlockCount;
                area.waterBlockCount += chunk.waterBlockCount;
                area.lavaBlockCount += chunk.lavaBlockCount;
            }
        }
    }
    return area;
}

// --- Presentation ----------------------------------------------------------

// Viewer framing and window chrome.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kTargetFps = 60;
constexpr float kCameraFovY = 55.0F;
constexpr float kCameraHeightAboveTerrain = 35.0F;
constexpr Color kBackground{12, 18, 27, 255};
constexpr Color kWireframe{150, 220, 210, 255};
constexpr Color kChunkBounds{70, 120, 130, 100};

// Start above and outside the area's highest surface, looking toward its center.
Camera3D framingCamera(const viewer::SmoothTerrainMesh& surface, int chunks) {
    const auto highest = std::max_element(
        surface.vertices.begin(), surface.vertices.end(),
        [](const viewer::VoxelVertex& a, const viewer::VoxelVertex& b) { return a.y < b.y; }
    );

    Camera3D camera{};
    const float center = static_cast<float>(chunks * 8 - (chunks / 2) * 16);
    const float distance = std::max(34.0F, chunks * 16.0F);
    camera.position = {center + distance, highest->y + kCameraHeightAboveTerrain + distance * 0.35F,
                       center + distance};
    camera.target = {center, highest->y, center};
    camera.up = {0.0F, 1.0F, 0.0F};
    camera.fovy = kCameraFovY;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
}

void drawOverlay(
    const Options& options,
    const viewer::SmoothTerrainMesh& surface,
    const viewer::VoxelMesh& voxels,
    double surfaceSeconds,
    double voxelSeconds,
    bool voxelMode,
    bool wireframe,
    bool faithfulTextures
) {
    DrawRectangle(18, 18, 780, 140, {5, 9, 15, 205});
    DrawText(
        TextFormat(
            "Seed %lld  |  Center chunk %d, %d  |  %d x %d chunks",
            static_cast<long long>(options.seed), options.chunkX, options.chunkZ, options.chunks, options.chunks
        ),
        32, 30, 21, RAYWHITE
    );
    DrawText(
        TextFormat("Terrain mesh: %zu triangles / %.2f s  |  Voxels: %zu faces / %.2f s",
                   surface.triangleCount(), surfaceSeconds, voxels.faceCount(), voxelSeconds),
        32, 58, 18, {150, 205, 200, 255}
    );
    DrawText(TextFormat("View: %s%s  |  %zu solid / %zu water / %zu lava blocks",
                        voxelMode ? "voxel blocks" : "smooth terrain", wireframe ? " wireframe" : "",
                        voxels.solidBlockCount, voxels.waterBlockCount, voxels.lavaBlockCount),
             32, 84, 18, RAYWHITE);
    DrawText("V: mesh/voxel | F: wireframe | WASD + mouse: fly | Shift: boost | TAB: cursor",
              32, 111, 16, {132, 151, 166, 255});
    DrawText(
        faithfulTextures
            ? (options.terrainOnly
                ? "Space/Ctrl: up/down | Faithful 32x textures | Stage 6 + 7A/7B terrain"
                : "Space/Ctrl: up/down | Faithful 32x textures | Stages 5-8 worldgen")
            : (options.terrainOnly
                ? "Space/Ctrl: up/down | Generated fallback textures | Stage 6 + 7A/7B terrain"
                : "Space/Ctrl: up/down | Generated fallback textures | Stages 5-8 worldgen"),
        32, 134, 14, {117, 148, 139, 255}
    );
    DrawFPS(GetScreenWidth() - 96, 20);
}

void runViewer(
    const Options& options,
    const viewer::SmoothTerrainMesh& surface,
    const viewer::VoxelMesh& voxels,
    double surfaceSeconds,
    double voxelSeconds
) {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWindowWidth, kWindowHeight, "Minecraft 26.3 Overworld terrain");
    if (!IsWindowReady()) {
        throw std::runtime_error("Could not initialize the viewer window");
    }
    SetTargetFPS(kTargetFps);
    DisableCursor();

    Model smoothModel = LoadModelFromMesh(viewer::uploadSmoothTerrainMesh(surface));
    Model voxelModel = LoadModelFromMesh(viewer::uploadVoxelMesh(voxels));
    const viewer::TerrainTextureAtlas atlas = viewer::loadTerrainTextureAtlas();
    SetMaterialTexture(&smoothModel.materials[0], MATERIAL_MAP_DIFFUSE, atlas.texture);
    SetMaterialTexture(&voxelModel.materials[0], MATERIAL_MAP_DIFFUSE, atlas.texture);
    Camera3D camera = framingCamera(surface, options.chunks);
    bool wireframe = false;
    bool voxelMode = options.voxel;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_F)) {
            wireframe = !wireframe;
        }
        if (IsKeyPressed(KEY_V)) {
            voxelMode = !voxelMode;
        }
        if (IsKeyPressed(KEY_TAB)) {
            IsCursorHidden() ? EnableCursor() : DisableCursor();
        }
        // Camera input pauses while the cursor is released.
        if (IsCursorHidden()) {
            const float speed = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 160.0F : 40.0F;
            Vector3 movement{
                static_cast<float>(IsKeyDown(KEY_W) - IsKeyDown(KEY_S)),
                static_cast<float>(IsKeyDown(KEY_D) - IsKeyDown(KEY_A)),
                static_cast<float>(IsKeyDown(KEY_SPACE) - IsKeyDown(KEY_LEFT_CONTROL))
            };
            movement = Vector3Scale(Vector3Normalize(movement), speed * GetFrameTime());
            const Vector2 mouse = GetMouseDelta();
            UpdateCameraPro(&camera, movement, {mouse.x * 0.1F, mouse.y * 0.1F, 0.0F}, 0.0F);
        }

        BeginDrawing();
        ClearBackground(kBackground);
        BeginMode3D(camera);
        const Model& model = voxelMode ? voxelModel : smoothModel;
        if (wireframe) {
            DrawModelWires(model, {0.0F, 0.0F, 0.0F}, 1.0F, kWireframe);
        } else {
            DrawModel(model, {0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        }
        DrawGrid(std::max(4, options.chunks * 2), 16.0F);
        const float low = static_cast<float>(-(options.chunks / 2) * 16);
        const float high = low + options.chunks * 16.0F;
        DrawBoundingBox({{low, -64.0F, low}, {high, 320.0F, high}}, kChunkBounds);
        EndMode3D();
        drawOverlay(
            options, surface, voxels, surfaceSeconds, voxelSeconds, voxelMode, wireframe, atlas.faithful
        );
        EndDrawing();
    }

    UnloadModel(voxelModel);
    UnloadModel(smoothModel);
    UnloadTexture(atlas.texture);
    CloseWindow();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);

        const mcworld::OverworldNoiseRouter router(options.seed);
        mcworld::GenerationOptions generation;
        if (!options.templates.empty() && !options.terrainOnly) {
            generation.templates = mcworld::loadStructureTemplateCatalog(options.templates);
            std::cout << "structure_templates=" << generation.templates->templates.size()
                      << " template_pools=" << generation.templates->pools.size()
                      << " unsupported_asset_semantics=" << generation.templates->unsupported.size() << '\n';
        }
        viewer::VoxelTerrain terrain(router, generation);

        const auto started = std::chrono::steady_clock::now();
        const int firstChunkX = options.chunkX - options.chunks / 2;
        const int firstChunkZ = options.chunkZ - options.chunks / 2;
        // Mesh extraction reads one neighboring chunk. Finalize that halo in a
        // single area so stage-8 writes crossing the visible edge are present.
        if (!options.terrainOnly) {
            terrain.prepareArea(firstChunkX - 1, firstChunkZ - 1, options.chunks + 2, options.chunks + 2);
        }
        const viewer::SmoothTerrainMesh surface =
            buildArea<viewer::SmoothTerrainMesh>(options, [&](int x, int z) {
                return terrain.buildSmoothMesh(x, z);
            });
        const double surfaceSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::cout << "seed=" << options.seed
                  << " chunk=(" << options.chunkX << ',' << options.chunkZ << ')'
                  << " chunks=" << options.chunks << 'x' << options.chunks
                  << " mesh_triangles=" << surface.triangleCount()
                   << " generation=" << surfaceSeconds << "s\n";

        if (surface.vertices.empty()) {
            std::cerr << "The selected area produced no terrain surface.\n";
            return 2;
        }
        if (options.headless && !options.voxel) {
            return 0;
        }

        const auto voxelStarted = std::chrono::steady_clock::now();
        const viewer::VoxelMesh voxels = buildArea<viewer::VoxelMesh>(options, [&](int x, int z) {
            return terrain.buildMesh(x, z);
        });
        const double voxelSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - voxelStarted).count();
        std::cout << "solid_blocks=" << voxels.solidBlockCount
                  << " water_blocks=" << voxels.waterBlockCount
                  << " lava_blocks=" << voxels.lavaBlockCount
                  << " voxel_faces=" << voxels.faceCount()
                  << " generation=" << voxelSeconds << "s\n";

        if (options.headless) {
            return 0;
        }

        runViewer(options, surface, voxels, surfaceSeconds, voxelSeconds);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain_viewer: " << error.what() << '\n';
        return 1;
    }
}

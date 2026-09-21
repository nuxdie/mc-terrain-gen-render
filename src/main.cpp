// Interactive viewer for the step 7A density field: generates a chunk area's
// density = 0 isosurface and either prints statistics (--headless) or opens a
// free-camera Raylib window on it.

#include "mcworld/isosurface.hpp"
#include "mcworld/worldgen.hpp"
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
        << "Usage: " << executable << " [--seed N] [--chunk-x N] [--chunk-z N] [--chunks N] [--voxel] [--headless]\n"
        << "\n"
        << "Renders the Minecraft 26.3 standard Overworld final-density zero surface.\n"
        << "--chunks N: N x N area around the selected chunk (default 8, range 1..16).\n"
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
        if (first * 16 < static_cast<std::int64_t>(std::numeric_limits<int>::min()) + 16
            || last * 16 > static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 32) {
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
            if constexpr (requires { area.positiveDensityVoxelCount; }) {
                area.positiveDensityVoxelCount += chunk.positiveDensityVoxelCount;
            }
        }
    }
    return area;
}

// --- Presentation ----------------------------------------------------------

// Terrain is tinted from a cold, dark base at the world floor to a pale tone at
// the build limit, then shaded by a single fixed key light. Purely cosmetic:
// none of this feeds back into generation.
constexpr float kColorFloorY = 64.0F;   // Offset that maps Y = -64 to 0.
constexpr float kColorSpanY = 240.0F;
constexpr float kAmbient = 0.38F;
constexpr float kDiffuse = 0.62F;
constexpr Vector3 kLightDirection{-0.45F, 0.82F, -0.35F};
constexpr Vector3 kColorLow{58.0F, 76.0F, 82.0F};
constexpr Vector3 kColorRange{112.0F, 106.0F, 100.0F};

// Viewer framing and window chrome.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kTargetFps = 60;
constexpr float kCameraFovY = 55.0F;
constexpr float kCameraHeightAboveTerrain = 35.0F;
constexpr Color kBackground{12, 18, 27, 255};
constexpr Color kWireframe{150, 220, 210, 255};
constexpr Color kChunkBounds{70, 120, 130, 100};

unsigned char colorChannel(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0F, 255.0F));
}

Mesh uploadMesh(const mcworld::SurfaceMesh& surface) {
    if (surface.vertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || surface.vertices.size() > std::numeric_limits<unsigned int>::max() / (3 * sizeof(float))) {
        throw std::runtime_error("Generated mesh exceeds Raylib's vertex limit");
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(surface.vertices.size());
    mesh.triangleCount = static_cast<int>(surface.triangleCount());
    mesh.vertices = static_cast<float*>(MemAlloc(surface.vertices.size() * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(surface.vertices.size() * 3 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(surface.vertices.size() * 4 * sizeof(unsigned char)));
    if (!mesh.vertices || !mesh.normals || !mesh.colors) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.colors);
        throw std::runtime_error("Could not allocate viewer mesh buffers");
    }

    const Vector3 light = Vector3Normalize(kLightDirection);
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const mcworld::SurfaceVertex& vertex = surface.vertices[i];
        mesh.vertices[i * 3] = vertex.x;
        mesh.vertices[i * 3 + 1] = vertex.y;
        mesh.vertices[i * 3 + 2] = vertex.z;
        mesh.normals[i * 3] = vertex.nx;
        mesh.normals[i * 3 + 1] = vertex.ny;
        mesh.normals[i * 3 + 2] = vertex.nz;

        const float height = std::clamp((vertex.y + kColorFloorY) / kColorSpanY, 0.0F, 1.0F);
        const float incidence = vertex.nx * light.x + vertex.ny * light.y + vertex.nz * light.z;
        const float lighting = kAmbient + kDiffuse * std::max(0.0F, incidence);
        mesh.colors[i * 4] = colorChannel((kColorLow.x + kColorRange.x * height) * lighting);
        mesh.colors[i * 4 + 1] = colorChannel((kColorLow.y + kColorRange.y * height) * lighting);
        mesh.colors[i * 4 + 2] = colorChannel((kColorLow.z + kColorRange.z * height) * lighting);
        mesh.colors[i * 4 + 3] = 255;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

// Start above and outside the area's highest surface, looking toward its center.
Camera3D framingCamera(const mcworld::SurfaceMesh& surface, int chunks) {
    const auto highest = std::max_element(
        surface.vertices.begin(), surface.vertices.end(),
        [](const mcworld::SurfaceVertex& a, const mcworld::SurfaceVertex& b) { return a.y < b.y; }
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
    const mcworld::SurfaceMesh& surface,
    const viewer::VoxelMesh& voxels,
    double surfaceSeconds,
    double voxelSeconds,
    bool voxelMode,
    bool wireframe
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
        TextFormat("Smooth: %zu triangles / %.2f s  |  Voxels: %zu faces / %.2f s",
                   surface.triangleCount(), surfaceSeconds, voxels.faceCount(), voxelSeconds),
        32, 58, 18, {150, 205, 200, 255}
    );
    DrawText(TextFormat("View: %s%s  |  %zu positive-density voxels",
                        voxelMode ? "voxel" : "smooth", wireframe ? " wireframe" : "",
                        voxels.positiveDensityVoxelCount),
             32, 84, 18, RAYWHITE);
    DrawText("V: smooth/voxel | F: wireframe | WASD + mouse: fly | Shift: boost | TAB: cursor",
             32, 111, 16, {132, 151, 166, 255});
    DrawText("Space/Ctrl: up/down | Fly: 40 blocks/s (Shift: 160) | Density only, no block materials",
             32, 134, 14, {117, 148, 139, 255});
    DrawFPS(GetScreenWidth() - 96, 20);
}

void runViewer(
    const Options& options,
    const mcworld::SurfaceMesh& surface,
    const viewer::VoxelMesh& voxels,
    double surfaceSeconds,
    double voxelSeconds
) {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWindowWidth, kWindowHeight, "Minecraft 26.3 density terrain");
    if (!IsWindowReady()) {
        throw std::runtime_error("Could not initialize the viewer window");
    }
    SetTargetFPS(kTargetFps);
    DisableCursor();

    Model smoothModel = LoadModelFromMesh(uploadMesh(surface));
    Model voxelModel = LoadModelFromMesh(viewer::uploadVoxelMesh(voxels));
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
        drawOverlay(options, surface, voxels, surfaceSeconds, voxelSeconds, voxelMode, wireframe);
        EndDrawing();
    }

    UnloadModel(voxelModel);
    UnloadModel(smoothModel);
    CloseWindow();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);

        const auto started = std::chrono::steady_clock::now();
        const mcworld::OverworldNoiseRouter router(options.seed);
        const mcworld::SurfaceMesh surface =
            buildArea<mcworld::SurfaceMesh>(options, [&](int x, int z) {
                return mcworld::buildChunkIsosurface(router, {x, z});
            });
        const double surfaceSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::cout << "seed=" << options.seed
                  << " chunk=(" << options.chunkX << ',' << options.chunkZ << ')'
                  << " chunks=" << options.chunks << 'x' << options.chunks
                  << " triangles=" << surface.triangleCount()
                   << " generation=" << surfaceSeconds << "s\n";

        if (surface.vertices.empty()) {
            std::cerr << "The selected area produced no density-zero surface.\n";
            return 2;
        }
        if (options.headless && !options.voxel) {
            return 0;
        }

        const auto voxelStarted = std::chrono::steady_clock::now();
        const viewer::VoxelMesh voxels = buildArea<viewer::VoxelMesh>(options, [&](int x, int z) {
            return viewer::buildVoxelMesh(router, x, z);
        });
        const double voxelSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - voxelStarted).count();
        std::cout << "positive_density_voxels=" << voxels.positiveDensityVoxelCount
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

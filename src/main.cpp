// Interactive viewer for the step 7A density field: generates one chunk's
// density = 0 isosurface and either prints statistics (--headless) or opens a
// free-camera Raylib window on it.

#include "mcworld/isosurface.hpp"
#include "mcworld/worldgen.hpp"

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
    bool headless{false};
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
        << "Usage: " << executable << " [--seed N] [--chunk-x N] [--chunk-z N] [--headless]\n"
        << "\n"
        << "Renders the Minecraft 26.3 standard Overworld final-density zero surface.\n";
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
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
        }
    }
    return options;
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

// Start just above and outside the tallest point of the chunk, looking at it.
Camera3D framingCamera(const mcworld::SurfaceMesh& surface) {
    const auto highest = std::max_element(
        surface.vertices.begin(), surface.vertices.end(),
        [](const mcworld::SurfaceVertex& a, const mcworld::SurfaceVertex& b) { return a.y < b.y; }
    );

    Camera3D camera{};
    camera.position = {42.0F, highest->y + kCameraHeightAboveTerrain, 42.0F};
    camera.target = {8.0F, highest->y, 8.0F};
    camera.up = {0.0F, 1.0F, 0.0F};
    camera.fovy = kCameraFovY;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
}

void drawOverlay(const Options& options, const mcworld::SurfaceMesh& surface, double elapsedSeconds) {
    DrawRectangle(18, 18, 460, 92, {5, 9, 15, 205});
    DrawText(
        TextFormat(
            "Seed %lld  |  Chunk %d, %d",
            static_cast<long long>(options.seed), options.chunkX, options.chunkZ
        ),
        32, 30, 21, RAYWHITE
    );
    DrawText(
        TextFormat("%zu triangles  |  %.2f s generation", surface.triangleCount(), elapsedSeconds),
        32, 58, 18, {150, 205, 200, 255}
    );
    DrawText("WASD + mouse: fly  |  F: wireframe  |  TAB: cursor", 32, 83, 16, {132, 151, 166, 255});
    DrawFPS(GetScreenWidth() - 96, 20);
}

void runViewer(const Options& options, const mcworld::SurfaceMesh& surface, double elapsedSeconds) {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWindowWidth, kWindowHeight, "Minecraft 26.3 density terrain");
    if (!IsWindowReady()) {
        throw std::runtime_error("Could not initialize the viewer window");
    }
    SetTargetFPS(kTargetFps);
    DisableCursor();

    Model model = LoadModelFromMesh(uploadMesh(surface));
    Camera3D camera = framingCamera(surface);
    bool wireframe = false;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_F)) {
            wireframe = !wireframe;
        }
        if (IsKeyPressed(KEY_TAB)) {
            IsCursorHidden() ? EnableCursor() : DisableCursor();
        }
        // Camera input pauses while the cursor is released.
        if (IsCursorHidden()) {
            UpdateCamera(&camera, CAMERA_FREE);
        }

        BeginDrawing();
        ClearBackground(kBackground);
        BeginMode3D(camera);
        if (wireframe) {
            DrawModelWires(model, {0.0F, 0.0F, 0.0F}, 1.0F, kWireframe);
        } else {
            DrawModel(model, {0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        }
        DrawGrid(64, 1.0F);
        DrawBoundingBox({{0.0F, -64.0F, 0.0F}, {16.0F, 320.0F, 16.0F}}, kChunkBounds);
        EndMode3D();
        drawOverlay(options, surface, elapsedSeconds);
        EndDrawing();
    }

    UnloadModel(model);
    CloseWindow();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);

        const auto started = std::chrono::steady_clock::now();
        const mcworld::OverworldNoiseRouter router(options.seed);
        const mcworld::SurfaceMesh surface =
            mcworld::buildChunkIsosurface(router, {options.chunkX, options.chunkZ});
        const double elapsedSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::cout << "seed=" << options.seed
                  << " chunk=(" << options.chunkX << ',' << options.chunkZ << ')'
                  << " triangles=" << surface.triangleCount()
                  << " generation=" << elapsedSeconds << "s\n";

        if (surface.vertices.empty()) {
            std::cerr << "The selected chunk produced no density-zero surface.\n";
            return 2;
        }
        if (options.headless) {
            return 0;
        }

        runViewer(options, surface, elapsedSeconds);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain_viewer: " << error.what() << '\n';
        return 1;
    }
}

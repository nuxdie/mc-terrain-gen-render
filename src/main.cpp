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
#include <string_view>

namespace {

struct Options {
    std::int64_t seed{0};
    int chunkX{0};
    int chunkZ{0};
    bool headless{false};
};

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

    const Vector3 light = Vector3Normalize({-0.45F, 0.82F, -0.35F});
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const mcworld::SurfaceVertex& vertex = surface.vertices[i];
        mesh.vertices[i * 3] = vertex.x;
        mesh.vertices[i * 3 + 1] = vertex.y;
        mesh.vertices[i * 3 + 2] = vertex.z;
        mesh.normals[i * 3] = vertex.nx;
        mesh.normals[i * 3 + 1] = vertex.ny;
        mesh.normals[i * 3 + 2] = vertex.nz;

        const float height = std::clamp((vertex.y + 64.0F) / 240.0F, 0.0F, 1.0F);
        const float diffuse = std::max(0.0F, vertex.nx * light.x + vertex.ny * light.y + vertex.nz * light.z);
        const float lighting = 0.38F + 0.62F * diffuse;
        const float red = (58.0F + 112.0F * height) * lighting;
        const float green = (76.0F + 106.0F * height) * lighting;
        const float blue = (82.0F + 100.0F * height) * lighting;
        mesh.colors[i * 4] = colorChannel(red);
        mesh.colors[i * 4 + 1] = colorChannel(green);
        mesh.colors[i * 4 + 2] = colorChannel(blue);
        mesh.colors[i * 4 + 3] = 255;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        mcworld::OverworldNoiseRouter router(options.seed);
        const mcworld::ChunkSurfaceRequest request{options.chunkX, options.chunkZ};
        const mcworld::SurfaceMesh surface = mcworld::buildChunkIsosurface(router, request);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);

        std::cout << "seed=" << options.seed
                  << " chunk=(" << options.chunkX << ',' << options.chunkZ << ')'
                  << " triangles=" << surface.triangleCount()
                  << " generation=" << elapsed.count() << "s\n";
        if (surface.vertices.empty()) {
            std::cerr << "The selected chunk produced no density-zero surface.\n";
            return 2;
        }
        if (options.headless) {
            return 0;
        }

        SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
        InitWindow(1280, 800, "Minecraft 26.3 density terrain");
        if (!IsWindowReady()) {
            throw std::runtime_error("Could not initialize the viewer window");
        }
        SetTargetFPS(60);
        DisableCursor();

        Mesh mesh = uploadMesh(surface);
        Model model = LoadModelFromMesh(mesh);
        Camera3D camera{};
        const auto highest = std::max_element(surface.vertices.begin(), surface.vertices.end(),
            [](const auto& a, const auto& b) { return a.y < b.y; });
        camera.position = {42.0F, highest->y + 35.0F, 42.0F};
        camera.target = {8.0F, highest->y, 8.0F};
        camera.up = {0.0F, 1.0F, 0.0F};
        camera.fovy = 55.0F;
        camera.projection = CAMERA_PERSPECTIVE;

        bool wireframe = false;
        while (!WindowShouldClose()) {
            if (IsKeyPressed(KEY_F)) {
                wireframe = !wireframe;
            }
            if (IsKeyPressed(KEY_TAB)) {
                IsCursorHidden() ? EnableCursor() : DisableCursor();
            }
            if (IsCursorHidden()) {
                UpdateCamera(&camera, CAMERA_FREE);
            }

            BeginDrawing();
            ClearBackground({12, 18, 27, 255});
            BeginMode3D(camera);
            if (wireframe) {
                DrawModelWires(model, {0.0F, 0.0F, 0.0F}, 1.0F, {150, 220, 210, 255});
            } else {
                DrawModel(model, {0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
            }
            DrawGrid(64, 1.0F);
            DrawBoundingBox({{0.0F, -64.0F, 0.0F}, {16.0F, 320.0F, 16.0F}}, {70, 120, 130, 100});
            EndMode3D();

            DrawRectangle(18, 18, 460, 92, {5, 9, 15, 205});
            DrawText(TextFormat("Seed %lld  |  Chunk %d, %d", static_cast<long long>(options.seed), options.chunkX, options.chunkZ), 32, 30, 21, RAYWHITE);
            DrawText(TextFormat("%zu triangles  |  %.2f s generation", surface.triangleCount(), elapsed.count()), 32, 58, 18, {150, 205, 200, 255});
            DrawText("WASD + mouse: fly  |  F: wireframe  |  TAB: cursor", 32, 83, 16, {132, 151, 166, 255});
            DrawFPS(GetScreenWidth() - 96, 20);
            EndDrawing();
        }

        UnloadModel(model);
        CloseWindow();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain_viewer: " << error.what() << '\n';
        return 1;
    }
}

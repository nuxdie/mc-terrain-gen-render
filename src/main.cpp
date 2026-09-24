// Headless Minecraft 26.3 Overworld generation and Sponge schematic export.
#include "mcworld/generation.hpp"
#include "mcworld/schematic.hpp"
#include "mcworld/structure_templates.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::int64_t seed{0};
    int chunkX{0};
    int chunkZ{0};
    int chunks{8};
    unsigned threads{std::clamp(std::thread::hardware_concurrency(), 1U, 8U)};
    bool terrainOnly{false};
#ifdef MCWORLD_DEFAULT_TEMPLATE_CATALOG
    std::string templates{MCWORLD_DEFAULT_TEMPLATE_CATALOG};
#else
    std::string templates;
#endif
    std::string exportPath;
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
        << "Usage: " << executable << " [--seed N] [--chunk-x N] [--chunk-z N] [--chunks N]\n"
        << "       [--threads N] [--terrain-only] [--templates FILE.mcwc] [--export FILE.schem]\n\n"
        << "Generates Minecraft 26.3 graph-level Overworld terrain without rendering.\n"
        << "--seed N: signed 64-bit world seed (default 0).\n"
        << "--chunk-x N, --chunk-z N: center chunk (default 0, 0).\n"
        << "--chunks N: N x N area around the selected chunk (default 8, range 1..16).\n"
        << "--threads N: terrain workers (default up to 8, range 1..64).\n"
        << "--terrain-only: skip structures and decoration.\n"
        << "--templates FILE: load an imported structure catalog (.mcwc).\n"
        << "--export FILE.schem: save the area for WorldEdit or voxel-viewer.\n"
        << "Without --export, generate and print statistics only.\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (argument == "--terrain-only") {
            options.terrainOnly = true;
            continue;
        }
        if (argument != "--seed" && argument != "--chunk-x" && argument != "--chunk-z"
            && argument != "--chunks" && argument != "--threads" && argument != "--templates"
            && argument != "--export") {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
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
        } else if (argument == "--threads") {
            options.threads = parseNumber<unsigned>(value, argument);
        } else if (argument == "--templates") {
            options.templates = value;
        } else if (argument == "--export") {
            if (value.empty()) {
                throw std::invalid_argument("--export requires a nonempty path");
            }
            options.exportPath = value;
        }
    }
    if (options.chunks < 1 || options.chunks > 16) {
        throw std::invalid_argument("--chunks must be between 1 and 16");
    }
    if (options.threads < 1 || options.threads > 64) {
        throw std::invalid_argument("--threads must be between 1 and 64");
    }
    // Check in wide arithmetic before calculating the first chunk as an int.
    // The generator validates its own sampling and dependency halos as well.
    for (const int center : {options.chunkX, options.chunkZ}) {
        const std::int64_t first = static_cast<std::int64_t>(center) - options.chunks / 2;
        const std::int64_t last = first + options.chunks - 1;
        if (first * 16 < static_cast<std::int64_t>(std::numeric_limits<int>::min()) + 512
            || last * 16 > static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 512) {
            throw std::invalid_argument("Chunk area is outside the supported integer grid");
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const mcworld::OverworldNoiseRouter router(options.seed);
        mcworld::GenerationOptions generation;
        generation.terrainThreads = options.threads;
        generation.structures = !options.terrainOnly;
        generation.features = !options.terrainOnly;
        if (!options.templates.empty() && !options.terrainOnly) {
            generation.templates = mcworld::loadStructureTemplateCatalog(options.templates);
            std::cout << "structure_templates=" << generation.templates->templates.size()
                      << " template_pools=" << generation.templates->pools.size()
                      << " unsupported_asset_semantics=" << generation.templates->unsupported.size() << '\n';
        }
        mcworld::OverworldWorldGenerator generator(router, generation);
        mcworld::GenerationProfile profile;
        const auto started = std::chrono::steady_clock::now();
        const auto area = generator.generateArea(
            options.chunkX - options.chunks / 2, options.chunkZ - options.chunks / 2,
            options.chunks, options.chunks, profile);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "world_generation=" << seconds << "s stage5=" << profile.stage5Seconds
                  << "s terrain=" << profile.terrainSeconds << "s decoration=" << profile.decorationSeconds
                  << "s harvest=" << profile.harvestSeconds << "s terrain_chunks=" << profile.terrainChunkCount
                  << " decoration_chunks=" << profile.decorationChunkCount
                  << " output_chunks=" << profile.outputChunkCount << '\n';
        const auto& detail = profile.terrainDetail;
        std::cout << "terrain_profile=biomes:" << detail.biomeSeconds
                  << "s density_aquifers:" << detail.densitySeconds << "s materials:" << detail.materialSeconds
                  << "s carvers:" << detail.carverSeconds << "s heightmaps:" << detail.heightmapSeconds
                  << "s fluid_cleanup:" << detail.fluidSeconds << "s chunks:" << detail.chunkCount << '\n';

        std::size_t solid = 0, water = 0, lava = 0;
        std::vector<const mcworld::TerrainChunk*> chunks;
        for (const auto& chunk : area.chunks) {
            chunks.push_back(&chunk.terrain);
            for (const auto block : chunk.terrain.blocks) {
                if (block == mcworld::Block::Water) {
                    ++water;
                } else if (block == mcworld::Block::Lava) {
                    ++lava;
                } else if (block != mcworld::Block::Air) {
                    ++solid;
                }
            }
        }
        std::cout << "seed=" << options.seed << " chunk=(" << options.chunkX << ',' << options.chunkZ
                  << ") chunks=" << options.chunks << 'x' << options.chunks
                  << " solid_blocks=" << solid << " water_blocks=" << water << " lava_blocks=" << lava << '\n';
        if (!options.exportPath.empty()) {
            mcworld::writeSpongeSchematic(options.exportPath, chunks, area.width, area.depth);
            std::cout << "exported_schematic=" << options.exportPath << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain_gen: " << error.what() << '\n';
        return 1;
    }
}

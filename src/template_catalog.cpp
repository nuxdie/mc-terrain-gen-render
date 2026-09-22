// Reading the structure-template catalog, and mapping Minecraft block IDs onto
// this port's much smaller material palette.
//
// The catalog is produced by tools/import_worldgen.py from a local game JAR
// and is not redistributed, so this file is also the only specification of
// that format; `loadStructureTemplateCatalog` below reads it top to bottom.

#include "mcworld/structure_templates.hpp"

#include <bit>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcworld {

// The render/terrain material a block state stands for. Every state's exact
// string survives in `BlockData`; this is only what the terrain and the mesher
// need to colour and collide with it.
//
// The table is the exact matches. Everything after it is a fallback chain
// ordered most specific first - `deepslate_bricks` has to reach the deepslate
// test before the brick one - so the tests below cannot be reordered or sorted.
Block templateMaterial(std::string_view state) {
    auto name = state.substr(0, state.find('['));
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    using enum Block;
    if (name == "cactus") return Cactus;
    for (auto plant : {"short_grass", "fern", "sweet_berry_bush", "cactus_flower", "dandelion", "poppy",
                       "azure_bluet", "oxeye_daisy", "cornflower", "orange_tulip", "red_tulip", "pink_tulip", "white_tulip"})
        if (name == plant) return Plant;
    static const std::unordered_map<std::string_view, Block> names{
        {"air", Air}, {"cave_air", Air}, {"void_air", Air}, {"water", Water}, {"lava", Lava},
        {"stone", Stone}, {"bedrock", Bedrock}, {"dirt", Dirt}, {"grass_block", Grass},
        {"coarse_dirt", CoarseDirt}, {"podzol", Podzol}, {"mycelium", Mycelium}, {"mud", Mud},
        {"dirt_path", Dirt}, {"farmland", Dirt}, {"sand", Sand}, {"sandstone", Sandstone},
        {"red_sand", RedSand}, {"red_sandstone", RedSandstone}, {"gravel", Gravel}, {"clay", Clay},
        {"snow", Snow}, {"snow_block", Snow}, {"powder_snow", PowderSnow}, {"ice", Ice},
        {"packed_ice", PackedIce}, {"blue_ice", PackedIce}, {"calcite", Calcite}, {"granite", Granite},
        {"diorite", Diorite}, {"andesite", Andesite}, {"deepslate", Deepslate}, {"tuff", Tuff},
        {"bricks", Bricks}, {"cobblestone", Cobblestone}, {"mossy_cobblestone", MossyCobblestone},
        {"coal_ore", CoalOre}, {"iron_ore", IronOre}, {"gold_ore", GoldOre}, {"diamond_ore", DiamondOre},
        {"redstone_ore", RedstoneOre}, {"lapis_ore", LapisOre}, {"copper_ore", CopperOre}, {"emerald_ore", EmeraldOre},
        {"deepslate_coal_ore", DeepslateCoalOre}, {"deepslate_iron_ore", DeepslateIronOre},
        {"deepslate_gold_ore", DeepslateGoldOre}, {"deepslate_diamond_ore", DeepslateDiamondOre},
        {"deepslate_redstone_ore", DeepslateRedstoneOre}, {"deepslate_lapis_ore", DeepslateLapisOre},
        {"deepslate_copper_ore", DeepslateCopperOre}, {"deepslate_emerald_ore", DeepslateEmeraldOre},
        {"terracotta", Terracotta}, {"white_terracotta", WhiteTerracotta}, {"orange_terracotta", OrangeTerracotta},
        {"yellow_terracotta", YellowTerracotta}, {"brown_terracotta", BrownTerracotta},
        {"red_terracotta", RedTerracotta}, {"light_gray_terracotta", LightGrayTerracotta},
    };
    if (const auto it = names.find(name); it != names.end()) return it->second;
    // The exact state is retained in BlockData. These are render/terrain
    // material fallbacks, not a substitute for Minecraft's block-state registry.
    if (name.ends_with("_log") || name.ends_with("_wood") || name.ends_with("_stem")) return OakLog;
    if (name.ends_with("_leaves") || name.find("sculk") != std::string_view::npos) return OakLeaves;
    if (name.find("deepslate") != std::string_view::npos || name == "obsidian" || name == "crying_obsidian") return Deepslate;
    if (name.find("sandstone") != std::string_view::npos) return name.starts_with("red_") ? RedSandstone : Sandstone;
    if (name.find("mossy") != std::string_view::npos) return MossyCobblestone;
    if (name.find("cobblestone") != std::string_view::npos) return Cobblestone;
    if (name.find("brick") != std::string_view::npos) return Stone;
    for (auto wood : {"oak", "spruce", "birch", "jungle", "acacia", "mangrove", "cherry", "bamboo", "pale_oak"})
        if (name.find(wood) != std::string_view::npos) return OakPlanks;
    if (name.find("glass") != std::string_view::npos) return Ice;
    if (name.find("snow") != std::string_view::npos) return Snow;
    if (name.find("wool") != std::string_view::npos || name.find("carpet") != std::string_view::npos) return WhiteTerracotta;
    if (name == "chest" || name == "barrel" || name == "bookshelf" || name == "crafting_table") return OakPlanks;
    return Stone;
}

namespace {

// A cursor over the catalog file. Every read is length- or range-checked, so a
// truncated or hand-edited file fails here rather than somewhere downstream
// with a plausible-looking structure.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file(path, std::ios::binary) {
        if (!file) throw std::runtime_error("Cannot open structure catalog: " + path.string());
    }

    void bytes(char* out, std::size_t count) {
        if (!file.read(out, static_cast<std::streamsize>(count))) throw std::runtime_error("Truncated structure catalog");
    }

    // All integers are little-endian and two's complement, independent of the
    // host, so that a catalog is portable between machines.
    int integer() {
        unsigned char data[4];
        bytes(reinterpret_cast<char*>(data), 4);
        const std::uint32_t value = data[0] | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
        return std::bit_cast<std::int32_t>(value);
    }

    // An integer used as a length or an enumerator, rejected unless it is in
    // [0, limit]. This is what bounds allocation from an untrusted file.
    int count(int limit = 1000000) {
        const int value = integer();
        if (value < 0 || value > limit) throw std::runtime_error("Invalid structure catalog count");
        return value;
    }

    bool boolean() {
        const int value = integer();
        if (value != 0 && value != 1) throw std::runtime_error("Invalid catalog boolean");
        return value != 0;
    }

    std::string text() {
        std::string value(static_cast<std::size_t>(count(4 * 1024 * 1024)), '\0');
        bytes(value.data(), value.size());
        return value;
    }

    BlockPosition position() {
        const int x = integer();
        const int y = integer();
        const int z = integer();
        return {x, y, z};
    }

    float real() { return std::bit_cast<float>(static_cast<std::uint32_t>(integer())); }

    void finish() {
        if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing structure catalog data");
    }

private:
    std::ifstream file;
};

// Block states are written once into a shared palette and referred to by index
// from every template, which is what keeps the file small: a village reuses
// the same few hundred states thousands of times.
using StatePalette = std::vector<std::shared_ptr<const BlockData>>;

[[nodiscard]] StatePalette readStatePalette(Reader& reader) {
    StatePalette palette;
    for (int n = reader.count(); n > 0; --n) {
        auto data = std::make_shared<BlockData>();
        data->state = reader.text();
        data->blockEntity = reader.text();
        palette.push_back(std::move(data));
    }
    return palette;
}

[[nodiscard]] StructureTemplate readTemplate(Reader& reader, const StatePalette& palette) {
    StructureTemplate result;
    result.size = reader.position();
    // A non-empty feature name marks a pool element that runs a feature
    // instead of placing blocks; it then carries none.
    result.feature = reader.text();

    for (int n = reader.count(); n > 0; --n) {
        const BlockPosition position = reader.position();
        const int state = reader.count();
        if (static_cast<std::size_t>(state) >= palette.size()) throw std::runtime_error("Template palette index out of range");
        result.blocks.push_back({position, templateMaterial(palette[state]->state), palette[state]});
    }

    for (int n = reader.count(); n > 0; --n) {
        TemplateConnector connector;
        connector.position = reader.position();
        connector.front = static_cast<TemplateDirection>(reader.count(5));
        connector.top = static_cast<TemplateDirection>(reader.count(5));
        connector.name = reader.text();
        connector.target = reader.text();
        connector.pool = reader.text();
        connector.rollable = reader.boolean();
        connector.selectionPriority = reader.integer();
        connector.placementPriority = reader.integer();
        connector.finalBlock = templateMaterial(reader.text());
        result.connectors.push_back(std::move(connector));
    }
    return result;
}

[[nodiscard]] TemplateProcessor readProcessor(Reader& reader) {
    TemplateProcessor processor;
    processor.kind = static_cast<TemplateProcessorKind>(reader.count(2));
    for (int n = reader.count(); n > 0; --n) processor.inputNames.push_back(reader.text());
    processor.outputState = reader.text();
    processor.output = templateMaterial(processor.outputState);
    processor.probability = reader.real();
    processor.ruleGroup = reader.count();
    return processor;
}

[[nodiscard]] StructureTemplatePool readPool(Reader& reader) {
    StructureTemplatePool pool;
    pool.fallback = reader.text();
    for (int n = reader.count(); n > 0; --n) {
        TemplatePoolElement element;
        element.templateId = reader.text();
        element.weight = reader.integer();
        // `NonPool` is this port's own marker for a stage-5 box and never
        // appears in a catalog, so only the two real projections are accepted.
        const int projection = reader.integer();
        if (projection < 1 || projection > 2) throw std::runtime_error("Invalid template projection");
        element.projection = static_cast<PieceProjection>(projection);
        for (int p = reader.count(); p > 0; --p) element.processors.push_back(readProcessor(reader));
        pool.elements.push_back(std::move(element));
    }
    return pool;
}

[[nodiscard]] TemplateStartPool readStart(Reader& reader) {
    TemplateStartPool start;
    start.pool = reader.text();
    start.maxDepth = reader.integer();
    start.maxDistance = reader.integer();
    start.startY = reader.integer();
    start.projectToSurface = reader.boolean();
    start.expansionHack = reader.boolean();
    start.startJigsawName = reader.text();
    return start;
}

} // namespace

std::shared_ptr<const StructureTemplateCatalog> loadStructureTemplateCatalog(const std::filesystem::path& path) {
    Reader reader(path);
    char magic[8];
    reader.bytes(magic, 8);
    if (std::string_view(magic, 8) != "MCWTPL01") throw std::runtime_error("Unsupported structure catalog format");

    auto catalog = std::make_shared<StructureTemplateCatalog>();
    // Pinned rather than range-checked: this port targets one game version,
    // and silently accepting another would produce a plausible wrong world.
    catalog->sourceVersion = reader.text();
    if (catalog->sourceVersion != "26.3") throw std::runtime_error("Structure catalog must target Minecraft 26.3");

    // Resources the importer saw but could not represent, kept so that callers
    // can report what a catalog is missing.
    for (int n = reader.count(); n > 0; --n) catalog->unsupported.push_back(reader.text());

    const StatePalette palette = readStatePalette(reader);

    for (int n = reader.count(100000); n > 0; --n) {
        const auto name = reader.text();
        if (!catalog->templates.emplace(name, readTemplate(reader, palette)).second) {
            throw std::runtime_error("Duplicate template ID");
        }
    }
    for (int n = reader.count(100000); n > 0; --n) {
        const auto name = reader.text();
        if (!catalog->pools.emplace(name, readPool(reader)).second) {
            throw std::runtime_error("Duplicate pool ID");
        }
    }
    for (int n = reader.count(100); n > 0; --n) {
        const auto variant = static_cast<StructureVariant>(reader.count(static_cast<int>(StructureVariant::AncientCity)));
        if (!catalog->starts.emplace(variant, readStart(reader)).second) {
            throw std::runtime_error("Duplicate start variant");
        }
    }

    reader.finish();
    // Cross-references are only resolvable once everything is read, so the
    // catalog is validated here and handed out immutable.
    catalog->validate();
    return catalog;
}

} // namespace mcworld

#include "mcworld/structure_templates.hpp"

#include <bit>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace mcworld {

Block templateMaterial(std::string_view state) {
    auto name = state.substr(0, state.find('['));
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    using enum Block;
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
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file(path, std::ios::binary) {
        if (!file) throw std::runtime_error("Cannot open structure catalog: " + path.string());
    }
    void bytes(char* out, std::size_t count) {
        if (!file.read(out, static_cast<std::streamsize>(count))) throw std::runtime_error("Truncated structure catalog");
    }
    int integer() {
        unsigned char data[4]; bytes(reinterpret_cast<char*>(data), 4);
        const std::uint32_t value = data[0] | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
        return std::bit_cast<std::int32_t>(value);
    }
    int count(int limit = 1000000) {
        const int value = integer();
        if (value < 0 || value > limit) throw std::runtime_error("Invalid structure catalog count");
        return value;
    }
    bool boolean() { const int v = integer(); if (v != 0 && v != 1) throw std::runtime_error("Invalid catalog boolean"); return v != 0; }
    std::string text() {
        std::string value(static_cast<std::size_t>(count(4 * 1024 * 1024)), '\0');
        bytes(value.data(), value.size()); return value;
    }
    BlockPosition position() { const int x = integer(), y = integer(), z = integer(); return {x, y, z}; }
    float real() { return std::bit_cast<float>(static_cast<std::uint32_t>(integer())); }
    void finish() { if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing structure catalog data"); }
private:
    std::ifstream file;
};
} // namespace

std::shared_ptr<const StructureTemplateCatalog> loadStructureTemplateCatalog(const std::filesystem::path& path) {
    Reader r(path);
    char magic[8]; r.bytes(magic, 8);
    if (std::string_view(magic, 8) != "MCWTPL01") throw std::runtime_error("Unsupported structure catalog format");
    auto catalog = std::make_shared<StructureTemplateCatalog>();
    catalog->sourceVersion = r.text();
    if (catalog->sourceVersion != "26.3") throw std::runtime_error("Structure catalog must target Minecraft 26.3");
    for (int n = r.count(); n > 0; --n) catalog->unsupported.push_back(r.text());
    std::vector<std::shared_ptr<const BlockData>> states;
    for (int n = r.count(); n > 0; --n) {
        auto data = std::make_shared<BlockData>(); data->state = r.text(); data->blockEntity = r.text(); states.push_back(std::move(data));
    }
    for (int n = r.count(100000); n > 0; --n) {
        const auto name = r.text();
        StructureTemplate t; t.size = r.position(); t.feature = r.text();
        for (int b = r.count(); b > 0; --b) {
            const auto p = r.position(); const int state = r.count();
            if (static_cast<std::size_t>(state) >= states.size()) throw std::runtime_error("Template palette index out of range");
            t.blocks.push_back({p, templateMaterial(states[state]->state), states[state]});
        }
        for (int j = r.count(); j > 0; --j) {
            TemplateConnector c; c.position = r.position();
            c.front = static_cast<TemplateDirection>(r.count(5)); c.top = static_cast<TemplateDirection>(r.count(5));
            c.name = r.text(); c.target = r.text(); c.pool = r.text(); c.rollable = r.boolean();
            c.selectionPriority = r.integer(); c.placementPriority = r.integer(); c.finalBlock = templateMaterial(r.text());
            t.connectors.push_back(std::move(c));
        }
        if (!catalog->templates.emplace(name, std::move(t)).second) throw std::runtime_error("Duplicate template ID");
    }
    for (int n = r.count(100000); n > 0; --n) {
        const auto name = r.text(); StructureTemplatePool pool; pool.fallback = r.text();
        for (int e = r.count(); e > 0; --e) {
            TemplatePoolElement element; element.templateId = r.text(); element.weight = r.integer();
            const int projection = r.integer();
            if (projection < 1 || projection > 2) throw std::runtime_error("Invalid template projection");
            element.projection = static_cast<PieceProjection>(projection);
            for (int p = r.count(); p > 0; --p) {
                TemplateProcessor processor; processor.kind = static_cast<TemplateProcessorKind>(r.count(2));
                for (int m = r.count(); m > 0; --m) processor.inputNames.push_back(r.text());
                processor.outputState = r.text(); processor.output = templateMaterial(processor.outputState);
                processor.probability = r.real(); processor.ruleGroup = r.count(); element.processors.push_back(std::move(processor));
            }
            pool.elements.push_back(std::move(element));
        }
        if (!catalog->pools.emplace(name, std::move(pool)).second) throw std::runtime_error("Duplicate pool ID");
    }
    for (int n = r.count(100); n > 0; --n) {
        const auto variant = static_cast<StructureVariant>(r.count(static_cast<int>(StructureVariant::AncientCity)));
        TemplateStartPool start; start.pool = r.text(); start.maxDepth = r.integer(); start.maxDistance = r.integer();
        start.startY = r.integer(); start.projectToSurface = r.boolean(); start.expansionHack = r.boolean(); start.startJigsawName = r.text();
        if (!catalog->starts.emplace(variant, std::move(start)).second) throw std::runtime_error("Duplicate start variant");
    }
    r.finish(); catalog->validate(); return catalog;
}
} // namespace mcworld

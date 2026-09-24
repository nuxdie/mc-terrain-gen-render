#include "mcworld/schematic.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcworld {
namespace {

// --- Binary NBT --------------------------------------------------------------

enum TagType : std::uint8_t {
    TagEnd = 0,
    TagShort = 2,
    TagInt = 3,
    TagByteArray = 7,
    TagString = 8,
    TagCompound = 10,
    TagIntArray = 11,
};

// Big-endian NBT writer into a byte buffer.
class NbtWriter {
public:
    std::vector<std::uint8_t> bytes;

    void beginCompound(std::string_view name) { header(TagCompound, name); }
    void endCompound() { byte(TagEnd); }

    void shortTag(std::string_view name, std::int16_t value) {
        header(TagShort, name);
        u16(static_cast<std::uint16_t>(value));
    }
    void intTag(std::string_view name, std::int32_t value) {
        header(TagInt, name);
        u32(static_cast<std::uint32_t>(value));
    }
    void intArrayTag(std::string_view name, std::initializer_list<std::int32_t> values) {
        header(TagIntArray, name);
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto value : values) u32(static_cast<std::uint32_t>(value));
    }
    void byteArrayTag(std::string_view name, const std::vector<std::uint8_t>& values) {
        header(TagByteArray, name);
        if (values.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::invalid_argument("Schematic block data is too large");
        }
        u32(static_cast<std::uint32_t>(values.size()));
        bytes.insert(bytes.end(), values.begin(), values.end());
    }

private:
    void byte(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) {
        byte(static_cast<std::uint8_t>(value >> 8));
        byte(static_cast<std::uint8_t>(value));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value >> 16));
        u16(static_cast<std::uint16_t>(value));
    }
    // NBT uses modified UTF-8; block-state identifiers are plain ASCII.
    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("NBT string is too long");
        }
        u16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void header(TagType type, std::string_view name) {
        byte(type);
        string(name);
    }
};

// --- gzip with stored deflate blocks -----------------------------------------

std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            result[i] = c;
        }
        return result;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto b : data) crc = table[(crc ^ b) & 0xFFU] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

void writeGzip(std::ostream& out, const std::vector<std::uint8_t>& data) {
    const auto put = [&out](std::uint8_t value) { out.put(static_cast<char>(value)); };
    const auto putLe32 = [&put](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) put(static_cast<std::uint8_t>(value >> shift));
    };
    // ID1 ID2 CM=deflate FLG=0 MTIME=0 XFL=0 OS=unknown
    for (const std::uint8_t b : {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}) put(b);

    constexpr std::size_t kMaxStored = 0xFFFF;
    std::size_t offset = 0;
    do {
        const std::size_t length = std::min(kMaxStored, data.size() - offset);
        const bool last = offset + length == data.size();
        put(last ? 1 : 0); // BFINAL, BTYPE=00 (stored)
        const auto len = static_cast<std::uint16_t>(length);
        put(static_cast<std::uint8_t>(len));
        put(static_cast<std::uint8_t>(len >> 8));
        put(static_cast<std::uint8_t>(~len));
        put(static_cast<std::uint8_t>(static_cast<std::uint16_t>(~len) >> 8));
        out.write(reinterpret_cast<const char*>(data.data() + offset), static_cast<std::streamsize>(length));
        offset += length;
    } while (offset < data.size());

    putLe32(crc32(data));
    putLe32(static_cast<std::uint32_t>(data.size()));
}

void writeVarint(std::vector<std::uint8_t>& out, std::uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::string namespaced(std::string_view state) {
    // Imported states may omit the default namespace.
    const auto bracket = state.find('[');
    const auto colon = state.find(':');
    if (colon != std::string_view::npos && colon < bracket) return std::string(state);
    return "minecraft:" + std::string(state);
}

} // namespace

std::string_view blockStateName(Block block) {
    switch (block) {
    case Block::Air: return "minecraft:air";
    case Block::Stone: return "minecraft:stone";
    case Block::Water: return "minecraft:water";
    case Block::Lava: return "minecraft:lava";
    case Block::Bedrock: return "minecraft:bedrock";
    case Block::Deepslate: return "minecraft:deepslate";
    case Block::Grass: return "minecraft:grass_block";
    case Block::Dirt: return "minecraft:dirt";
    case Block::Sand: return "minecraft:sand";
    case Block::Sandstone: return "minecraft:sandstone";
    case Block::RedSand: return "minecraft:red_sand";
    case Block::RedSandstone: return "minecraft:red_sandstone";
    case Block::Gravel: return "minecraft:gravel";
    case Block::Terracotta: return "minecraft:terracotta";
    case Block::WhiteTerracotta: return "minecraft:white_terracotta";
    case Block::OrangeTerracotta: return "minecraft:orange_terracotta";
    case Block::Podzol: return "minecraft:podzol";
    case Block::CoarseDirt: return "minecraft:coarse_dirt";
    case Block::Mycelium: return "minecraft:mycelium";
    case Block::Mud: return "minecraft:mud";
    case Block::Snow: return "minecraft:snow_block";
    case Block::PowderSnow: return "minecraft:powder_snow";
    case Block::Ice: return "minecraft:ice";
    case Block::PackedIce: return "minecraft:packed_ice";
    case Block::Calcite: return "minecraft:calcite";
    case Block::CopperOre: return "minecraft:copper_ore";
    case Block::RawCopper: return "minecraft:raw_copper_block";
    case Block::Granite: return "minecraft:granite";
    case Block::DeepslateIronOre: return "minecraft:deepslate_iron_ore";
    case Block::RawIron: return "minecraft:raw_iron_block";
    case Block::Tuff: return "minecraft:tuff";
    case Block::Sulfur: return "minecraft:sulfur";
    case Block::Cinnabar: return "minecraft:cinnabar";
    case Block::YellowTerracotta: return "minecraft:yellow_terracotta";
    case Block::BrownTerracotta: return "minecraft:brown_terracotta";
    case Block::RedTerracotta: return "minecraft:red_terracotta";
    case Block::LightGrayTerracotta: return "minecraft:light_gray_terracotta";
    case Block::CoalOre: return "minecraft:coal_ore";
    case Block::IronOre: return "minecraft:iron_ore";
    case Block::GoldOre: return "minecraft:gold_ore";
    case Block::RedstoneOre: return "minecraft:redstone_ore";
    case Block::DiamondOre: return "minecraft:diamond_ore";
    case Block::LapisOre: return "minecraft:lapis_ore";
    case Block::Clay: return "minecraft:clay";
    case Block::OakLog: return "minecraft:oak_log";
    case Block::OakLeaves: return "minecraft:oak_leaves";
    case Block::OakPlanks: return "minecraft:oak_planks";
    case Block::Cobblestone: return "minecraft:cobblestone";
    case Block::MossyCobblestone: return "minecraft:mossy_cobblestone";
    case Block::Bricks: return "minecraft:bricks";
    case Block::Diorite: return "minecraft:diorite";
    case Block::Andesite: return "minecraft:andesite";
    case Block::EmeraldOre: return "minecraft:emerald_ore";
    case Block::DeepslateCoalOre: return "minecraft:deepslate_coal_ore";
    case Block::DeepslateGoldOre: return "minecraft:deepslate_gold_ore";
    case Block::DeepslateRedstoneOre: return "minecraft:deepslate_redstone_ore";
    case Block::DeepslateDiamondOre: return "minecraft:deepslate_diamond_ore";
    case Block::DeepslateLapisOre: return "minecraft:deepslate_lapis_ore";
    case Block::DeepslateCopperOre: return "minecraft:deepslate_copper_ore";
    case Block::DeepslateEmeraldOre: return "minecraft:deepslate_emerald_ore";
    case Block::Plant: return "minecraft:short_grass";
    case Block::Cactus: return "minecraft:cactus";
    }
    return "minecraft:air";
}

void writeSpongeSchematic(
    std::ostream& out,
    const std::vector<const TerrainChunk*>& chunks,
    int width,
    int depth,
    const SchematicExportOptions& options
) {
    if (width < 1 || depth < 1 || chunks.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(depth)) {
        throw std::invalid_argument("Schematic area does not match its chunk list");
    }
    if (static_cast<std::int64_t>(width) * TerrainChunk::width > std::numeric_limits<std::uint16_t>::max()
        || static_cast<std::int64_t>(depth) * TerrainChunk::width > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("Schematic area is too large");
    }
    for (const auto* chunk : chunks) {
        if (chunk == nullptr) throw std::invalid_argument("Schematic chunk list contains a null chunk");
    }
    const int firstChunkX = chunks.front()->chunkX;
    const int firstChunkZ = chunks.front()->chunkZ;
    for (int z = 0; z < depth; ++z) {
        for (int x = 0; x < width; ++x) {
            const auto* chunk = chunks[static_cast<std::size_t>(z) * width + x];
            if (chunk->chunkX != firstChunkX + x || chunk->chunkZ != firstChunkZ + z) {
                throw std::invalid_argument("Schematic chunks must be a contiguous row-major rectangle");
            }
        }
    }

    int minY = std::max(options.minY, TerrainChunk::minY);
    int maxY = std::min(options.maxY, TerrainChunk::maxY);
    if (options.trimAir) {
        int lowest = maxY;
        int highest = minY - 1;
        for (const auto* chunk : chunks) {
            for (int z = 0; z < TerrainChunk::width; ++z) {
                for (int x = 0; x < TerrainChunk::width; ++x) {
                    for (int y = minY; y < maxY; ++y) {
                        if (chunk->at(x, y, z) != Block::Air) {
                            lowest = std::min(lowest, y);
                            highest = std::max(highest, y);
                        }
                    }
                }
            }
        }
        if (highest >= lowest) {
            minY = lowest;
            maxY = highest + 1;
        } else {
            maxY = minY + 1; // all air: keep a one-block-high schematic
        }
    }
    if (maxY <= minY) throw std::invalid_argument("Schematic Y range is empty");

    const int sizeX = width * TerrainChunk::width;
    const int sizeZ = depth * TerrainChunk::width;
    const int sizeY = maxY - minY;

    // Palette ids are assigned in first-seen order, which Sponge allows.
    std::unordered_map<std::string, std::uint32_t> palette;
    std::vector<const std::string*> paletteOrder;
    const auto paletteId = [&](std::string name) {
        const auto [it, inserted] = palette.try_emplace(std::move(name), static_cast<std::uint32_t>(palette.size()));
        if (inserted) paletteOrder.push_back(&it->first);
        return it->second;
    };
    paletteId("minecraft:air");

    std::vector<std::uint8_t> blockData;
    blockData.reserve(static_cast<std::size_t>(sizeX) * sizeY * sizeZ);
    // Sponge order: index = x + z * Width + y * Width * Length.
    for (int y = minY; y < maxY; ++y) {
        for (int z = 0; z < sizeZ; ++z) {
            const int chunkRow = (z / TerrainChunk::width) * width;
            const int localZ = z % TerrainChunk::width;
            for (int x = 0; x < sizeX; ++x) {
                const auto& chunk = *chunks[static_cast<std::size_t>(chunkRow + x / TerrainChunk::width)];
                const int localX = x % TerrainChunk::width;
                const Block block = chunk.at(localX, y, localZ);
                std::uint32_t id = 0;
                if (const auto* data = chunk.dataAt(localX, y, localZ); data != nullptr && !data->state.empty()) {
                    id = paletteId(namespaced(data->state));
                } else if (block != Block::Air) {
                    id = paletteId(std::string(blockStateName(block)));
                }
                writeVarint(blockData, id);
            }
        }
    }

    const std::int64_t originX = static_cast<std::int64_t>(firstChunkX) * TerrainChunk::width;
    const std::int64_t originZ = static_cast<std::int64_t>(firstChunkZ) * TerrainChunk::width;
    if (originX < std::numeric_limits<std::int32_t>::min() || originX > std::numeric_limits<std::int32_t>::max()
        || originZ < std::numeric_limits<std::int32_t>::min() || originZ > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument("Schematic origin is outside the integer grid");
    }

    NbtWriter nbt;
    nbt.beginCompound("Schematic");
    nbt.intTag("Version", 2);
    nbt.intTag("DataVersion", options.dataVersion);
    nbt.shortTag("Width", static_cast<std::int16_t>(static_cast<std::uint16_t>(sizeX)));
    nbt.shortTag("Height", static_cast<std::int16_t>(static_cast<std::uint16_t>(sizeY)));
    nbt.shortTag("Length", static_cast<std::int16_t>(static_cast<std::uint16_t>(sizeZ)));
    nbt.intArrayTag("Offset", {static_cast<std::int32_t>(originX), minY, static_cast<std::int32_t>(originZ)});
    nbt.beginCompound("Metadata");
    nbt.intTag("WEOffsetX", 0);
    nbt.intTag("WEOffsetY", 0);
    nbt.intTag("WEOffsetZ", 0);
    nbt.endCompound();
    nbt.intTag("PaletteMax", static_cast<std::int32_t>(palette.size()));
    nbt.beginCompound("Palette");
    for (std::size_t id = 0; id < paletteOrder.size(); ++id) {
        nbt.intTag(*paletteOrder[id], static_cast<std::int32_t>(id));
    }
    nbt.endCompound();
    nbt.byteArrayTag("BlockData", blockData);
    nbt.endCompound();

    writeGzip(out, nbt.bytes);
    if (!out) throw std::runtime_error("Failed to write schematic");
}

void writeSpongeSchematic(std::ostream& out, const GeneratedArea& area, const SchematicExportOptions& options) {
    std::vector<const TerrainChunk*> chunks;
    chunks.reserve(area.chunks.size());
    for (const auto& chunk : area.chunks) chunks.push_back(&chunk.terrain);
    writeSpongeSchematic(out, chunks, area.width, area.depth, options);
}

void writeSpongeSchematic(
    const std::filesystem::path& path,
    const std::vector<const TerrainChunk*>& chunks,
    int width,
    int depth,
    const SchematicExportOptions& options
) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Could not open " + path.string() + " for writing");
    writeSpongeSchematic(file, chunks, width, depth, options);
    file.close();
    if (!file) throw std::runtime_error("Failed to write " + path.string());
}

} // namespace mcworld

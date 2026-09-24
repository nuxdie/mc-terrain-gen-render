#include "mcworld/schematic.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Inflates the stored-block gzip stream the writer produces, verifying CRC and size.
std::vector<std::uint8_t> gunzipStored(const std::string& file) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(file.data());
    if (file.size() < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 8) throw std::runtime_error("not gzip");
    std::size_t pos = 10;
    std::vector<std::uint8_t> out;
    for (bool last = false; !last;) {
        last = (b[pos] & 1) != 0;
        if ((b[pos] >> 1) != 0) throw std::runtime_error("unexpected block type");
        const std::uint16_t len = static_cast<std::uint16_t>(b[pos + 1] | (b[pos + 2] << 8));
        const std::uint16_t nlen = static_cast<std::uint16_t>(b[pos + 3] | (b[pos + 4] << 8));
        if (static_cast<std::uint16_t>(~len) != nlen) throw std::runtime_error("bad stored length");
        out.insert(out.end(), b + pos + 5, b + pos + 5 + len);
        pos += 5 + len;
    }
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : out) {
        crc ^= byte;
        for (int k = 0; k < 8; ++k) crc = (crc & 1U) ? 0xEDB88320U ^ (crc >> 1) : crc >> 1;
    }
    crc ^= 0xFFFFFFFFU;
    const auto le32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16)) | (std::uint32_t{b[at + 3]} << 24);
    };
    if (le32(pos) != crc) throw std::runtime_error("bad crc");
    if (le32(pos + 4) != out.size()) throw std::runtime_error("bad isize");
    if (pos + 8 != file.size()) throw std::runtime_error("trailing bytes");
    return out;
}

// Minimal NBT reader for the tags the writer emits.
struct Nbt {
    std::map<std::string, std::int64_t> numbers;
    std::map<std::string, std::vector<std::int32_t>> intArrays;
    std::vector<std::uint8_t> blockData;
    std::map<std::string, std::int32_t> palette;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
    Nbt parse() {
        Nbt result;
        if (u8() != 10 || string() != "Schematic") throw std::runtime_error("bad root");
        compound(result, "");
        if (pos_ != data_.size()) throw std::runtime_error("trailing NBT");
        return result;
    }

private:
    std::uint8_t u8() { return data_.at(pos_++); }
    std::uint16_t u16() { return static_cast<std::uint16_t>((u8() << 8) | u8()); }
    std::int32_t i32() { return static_cast<std::int32_t>((std::uint32_t{u16()} << 16) | u16()); }
    std::string string() {
        const auto n = u16();
        std::string s(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return s;
    }
    void compound(Nbt& out, const std::string& path) {
        for (;;) {
            const auto type = u8();
            if (type == 0) return;
            const auto name = string();
            switch (type) {
            case 2: out.numbers[path + name] = static_cast<std::int16_t>(u16()); break;
            case 3:
                if (path == "Palette.") out.palette[name] = i32();
                else out.numbers[path + name] = i32();
                break;
            case 7: {
                const auto n = static_cast<std::size_t>(i32());
                out.blockData.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
                pos_ += n;
                break;
            }
            case 10: compound(out, path + name + "."); break;
            case 11: {
                const auto n = i32();
                auto& values = out.intArrays[path + name];
                for (int i = 0; i < n; ++i) values.push_back(i32());
                break;
            }
            default: throw std::runtime_error("unexpected tag type");
            }
        }
    }
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_{};
};

std::vector<std::uint32_t> decodeVarints(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> values;
    std::uint32_t value = 0;
    int shift = 0;
    for (const auto b : bytes) {
        value |= std::uint32_t{b & 0x7FU} << shift;
        if (b & 0x80U) {
            shift += 7;
        } else {
            values.push_back(value);
            value = 0;
            shift = 0;
        }
    }
    return values;
}

void testRoundTrip() {
    mcworld::TerrainChunk a;
    a.chunkX = -3;
    a.chunkZ = 5;
    mcworld::TerrainChunk b = a;
    b.chunkX = -2;
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            a.set(x, 10, z, mcworld::Block::Stone);
            b.set(x, 10, z, mcworld::Block::Grass);
        }
    a.set(3, 12, 4, mcworld::Block::Water);
    b.set(1, 11, 2, mcworld::Block::Plant);
    b.setData(1, 11, 2, std::make_shared<mcworld::BlockData>(mcworld::BlockData{"minecraft:poppy", {}}));
    b.set(15, 13, 15, mcworld::Block::OakLog);
    b.setData(15, 13, 15, std::make_shared<mcworld::BlockData>(mcworld::BlockData{"oak_log[axis=x]", {}}));

    std::ostringstream stream;
    mcworld::writeSpongeSchematic(stream, {&a, &b}, 2, 1);
    const Nbt nbt = Reader(gunzipStored(stream.str())).parse();

    check(nbt.numbers.at("Version") == 2, "sponge version 2");
    check(nbt.numbers.at("Width") == 32 && nbt.numbers.at("Length") == 16, "horizontal size");
    check(nbt.numbers.at("Height") == 4, "height trimmed to y 10..13");
    check((nbt.intArrays.at("Offset") == std::vector<std::int32_t>{-48, 10, 80}), "world offset");
    check(nbt.numbers.at("PaletteMax") == static_cast<std::int64_t>(nbt.palette.size()), "palette max");
    check(nbt.palette.at("minecraft:air") == 0, "air is palette zero");
    check(nbt.palette.count("minecraft:oak_log[axis=x]") == 1, "imported state is namespaced");
    check(nbt.palette.count("minecraft:short_grass") == 0, "block data overrides material name");

    const auto values = decodeVarints(nbt.blockData);
    check(values.size() == 32U * 4U * 16U, "one entry per block");
    const auto at = [&](int x, int y, int z) { return values.at(static_cast<std::size_t>(x + z * 32 + y * 32 * 16)); };
    check(at(0, 0, 0) == static_cast<std::uint32_t>(nbt.palette.at("minecraft:stone")), "stone in first chunk");
    check(at(20, 0, 7) == static_cast<std::uint32_t>(nbt.palette.at("minecraft:grass_block")), "grass in second chunk");
    check(at(3, 2, 4) == static_cast<std::uint32_t>(nbt.palette.at("minecraft:water")), "water position");
    check(at(17, 1, 2) == static_cast<std::uint32_t>(nbt.palette.at("minecraft:poppy")), "imported plant state");
    check(at(31, 3, 15) == static_cast<std::uint32_t>(nbt.palette.at("minecraft:oak_log[axis=x]")), "last block");
    check(at(0, 1, 0) == 0, "air above stone");

    std::ostringstream full;
    mcworld::SchematicExportOptions options;
    options.trimAir = false;
    mcworld::writeSpongeSchematic(full, {&a}, 1, 1, options);
    const Nbt fullNbt = Reader(gunzipStored(full.str())).parse();
    check(fullNbt.numbers.at("Height") == mcworld::TerrainChunk::height, "untrimmed height");
    check(fullNbt.intArrays.at("Offset").at(1) == mcworld::TerrainChunk::minY, "untrimmed offset");
}

void testInvalidAreas() {
    mcworld::TerrainChunk a;
    std::ostringstream stream;
    bool threw = false;
    try {
        mcworld::writeSpongeSchematic(stream, {&a}, 2, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "chunk count must match area");
    mcworld::TerrainChunk far = a;
    far.chunkX = 7;
    threw = false;
    try {
        mcworld::writeSpongeSchematic(stream, {&a, &far}, 2, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "chunks must be contiguous");
}

void testBlockNames() {
    check(mcworld::blockStateName(mcworld::Block::Grass) == "minecraft:grass_block", "grass name");
    check(mcworld::blockStateName(mcworld::Block::Cactus) == "minecraft:cactus", "last enum value is named");
}
} // namespace

int main() {
    try {
        testRoundTrip();
        testInvalidAreas();
        testBlockNames();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    }
    if (failures) std::cerr << failures << " schematic checks failed\n";
    else std::cout << "All schematic checks passed\n";
    return failures ? 1 : 0;
}

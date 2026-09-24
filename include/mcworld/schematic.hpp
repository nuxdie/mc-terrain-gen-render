#pragma once

// Exports generated chunks as a Sponge schematic (`.schem`, format version 2),
// the format WorldEdit uses for 1.13+ and one that voxel viewers such as
// nuxdie/voxel-viewer open directly.
//
// The writer has no dependencies: the NBT payload is wrapped in a valid gzip
// stream made of stored (uncompressed) deflate blocks. Files are therefore
// larger than WorldEdit's own output, but every gzip reader accepts them.

#include "mcworld/generation.hpp"
#include "mcworld/terrain.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace mcworld {

struct SchematicExportOptions {
    // Inclusive-exclusive world Y range to export. Clamped to the chunk range.
    int minY = TerrainChunk::minY;
    int maxY = TerrainChunk::maxY;
    // Shrinks the exported Y range to the lowest and highest non-air blocks.
    bool trimAir = true;
    // Minecraft data version recorded in the file. Readers that upgrade block
    // states (WorldEdit) treat the palette as coming from this version.
    std::int32_t dataVersion = 4189;
};

// The namespaced block-state identifier used for a material that carries no
// imported `BlockData`, for example `minecraft:grass_block`.
[[nodiscard]] std::string_view blockStateName(Block block);

// Writes the rectangle of chunks as one schematic. `chunks` is row-major in Z
// and then X, like `GeneratedArea::chunks`, and must hold exactly
// `width * depth` non-null chunks. The schematic origin (0, 0, 0) is the
// lowest exported block of the first chunk; its world position is stored in
// the `Offset` field and in the WorldEdit origin metadata.
//
// Throws `std::invalid_argument` for a malformed area and
// `std::runtime_error` if the stream fails.
void writeSpongeSchematic(
    std::ostream& out,
    const std::vector<const TerrainChunk*>& chunks,
    int width,
    int depth,
    const SchematicExportOptions& options = {}
);

void writeSpongeSchematic(std::ostream& out, const GeneratedArea& area, const SchematicExportOptions& options = {});

// Convenience overload that writes to a file, replacing it.
void writeSpongeSchematic(
    const std::filesystem::path& path,
    const std::vector<const TerrainChunk*>& chunks,
    int width,
    int depth,
    const SchematicExportOptions& options = {}
);

} // namespace mcworld

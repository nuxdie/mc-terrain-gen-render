#pragma once

#include "mcworld/terrain.hpp"

#include <algorithm>
#include <array>

namespace mcworld::detail {

[[nodiscard]] float structureBeardContribution(int dx, int dy, int dz, int yToGround);

// MiscOverworldFeatures.SPRING_WATER, restricted to our material palette.
[[nodiscard]] inline bool springRock(Block block) {
    using enum Block;
    return block == Stone || block == Granite || block == Deepslate || block == Tuff
        || block == Calcite || block == Dirt || block == Snow || block == PowderSnow
        || block == PackedIce;
}

// SpringFeature counts the four horizontal neighbors and below, not above.
[[nodiscard]] inline bool canPlaceWaterSpring(
    Block current, Block above, Block below, const std::array<Block, 4>& sides
) {
    if (!springRock(above) || !springRock(below)
        || (current != Block::Air && !springRock(current))) return false;
    const int rocks = 1 + std::count_if(sides.begin(), sides.end(), springRock);
    const int holes = std::count(sides.begin(), sides.end(), Block::Air);
    return rocks == 4 && holes == 1;
}

} // namespace mcworld::detail

#pragma once

// Java's `java.util.Random` (`LegacyRandomSource` in Minecraft), plus the two
// seed helpers the terrain stages derive their positional streams from.
//
// Every value produced here feeds terrain decisions, so the bit layout is the
// contract: the 48-bit LCG constants, the number of bits each draw consumes,
// the rejection loop in `nextInt`, and the sign extension in `nextLong` all
// have to match Java exactly. Changing how many bits a call consumes shifts
// every later draw in the same stream and regenerates the world.

#include "noise.hpp"

#include <cstdint>

namespace mcworld::detail {

// Java's `Math.floorDiv`: rounds towards negative infinity, unlike C++'s `/`.
// Terrain grids extend into negative coordinates, where the difference decides
// which cell a block belongs to.
[[nodiscard]] inline int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return quotient - (value % divisor < 0);
}

class LegacyRandom {
public:
    explicit LegacyRandom(std::uint64_t seed) : state_((seed ^ kMultiplier) & kMask) {}

    // Top `count` bits of the next LCG state, the primitive all draws build on.
    std::uint32_t bits(int count) {
        state_ = (state_ * kMultiplier + kIncrement) & kMask;
        return static_cast<std::uint32_t>(state_ >> (48 - count));
    }

    // Java's `nextInt(bound)`: a fast path for powers of two, and otherwise a
    // rejection loop that redraws on the values that would bias the modulus.
    int nextInt(int bound) {
        if ((bound & -bound) == bound) {
            return static_cast<int>((static_cast<std::uint64_t>(bound) * bits(31)) >> 31);
        }
        std::uint32_t value = 0;
        std::uint32_t remainder = 0;
        do {
            value = bits(31);
            remainder = value % static_cast<std::uint32_t>(bound);
        } while (value - remainder + static_cast<std::uint32_t>(bound - 1) >= 0x80000000U);
        return static_cast<int>(remainder);
    }

    float nextFloat() { return static_cast<float>(bits(24)) * 0x1p-24F; }

    double nextDouble() {
        const std::uint64_t high = bits(26);
        const std::uint64_t low = bits(27);
        return static_cast<double>((high << 27) + low) * 0x1p-53;
    }

    // Java's `nextLong` sign-extends the low half before adding it, so the two
    // halves are not independent: the low draw can borrow from the high one.
    std::uint64_t nextLong() {
        const std::uint64_t high = bits(32);
        const auto low = static_cast<std::int64_t>(static_cast<std::int32_t>(bits(32)));
        return (high << 32) + static_cast<std::uint64_t>(low);
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5deece66dULL;
    static constexpr std::uint64_t kIncrement = 11;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;

    std::uint64_t state_;
};

// Deterministic per-position seed for a named decision (ore placement, bedrock
// roughness, per-column surface jitter).
//
// This is the project's own convention rather than Minecraft's positional
// random factory, which is one of the documented reasons chunks differ from
// vanilla for the same numeric seed. It still has to stay stable: rewriting the
// mixing here changes every seeded material decision in the world.
[[nodiscard]] inline std::uint64_t positionalSeed(std::int64_t worldSeed, std::string_view key, int x, int y, int z) {
    constexpr std::uint64_t kYSalt = 0x9e3779b97f4a7c15ULL;
    constexpr std::uint64_t kZSalt = 0xd1b54a32d192ed03ULL;
    auto mix = [](std::uint64_t value) {
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    };
    return mix(deriveSeed(worldSeed, key) ^ mix(static_cast<std::uint64_t>(x)) ^
               mix(static_cast<std::uint64_t>(y) + kYSalt) ^ mix(static_cast<std::uint64_t>(z) + kZSalt));
}

} // namespace mcworld::detail

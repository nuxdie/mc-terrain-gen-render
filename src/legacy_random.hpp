#pragma once

// Java's `RandomSource` implementations the generator uses - the legacy
// 48-bit LCG (`java.util.Random`) and the xoroshiro source behind
// `WorldgenRandom` - plus the seed helpers the stages derive their positional
// streams from.
//
// Every value produced here feeds a generation decision, so the bit layout is
// the contract: the LCG constants, the number of bits each draw consumes, the
// rejection loop in `nextInt`, and the sign extension in `nextLong` all have
// to match Java exactly. Changing how many bits a call consumes shifts every
// later draw in the same stream and regenerates the world.
//
// In Java the draw algorithms are default methods on `RandomSource`, shared by
// both implementations; `RandomDraws` below is that sharing, so the two sources
// cannot drift apart.

#include "noise.hpp"

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace mcworld::detail {

// Java's `Math.floorDiv`: rounds towards negative infinity, unlike C++'s `/`.
// Terrain grids extend into negative coordinates, where the difference decides
// which cell a block belongs to.
[[nodiscard]] inline int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return quotient - (value % divisor < 0);
}

// The splitmix64 finalizer (Stafford variant 13) Java uses to scramble a seed
// into well-distributed bits. Shared by the xoroshiro seeding below and by this
// project's own positional seeds.
[[nodiscard]] inline std::uint64_t mixStafford13(std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

// Java's `RandomSource` draw algorithms, in terms of the one primitive its
// implementations differ in: `Source::bits(count)`, the next `count` random
// bits. CRTP rather than virtual dispatch because these are called per block.
template <typename Source>
class RandomDraws {
public:
    // Java's `nextInt(bound)`: a fast path for powers of two, and otherwise a
    // rejection loop that redraws on the values that would bias the modulus.
    int nextInt(int bound) {
        if (bound <= 0) {
            throw std::invalid_argument("Random bound must be positive");
        }
        if ((bound & -bound) == bound) {
            return static_cast<int>((static_cast<std::uint64_t>(bound) * nextBits(31)) >> 31);
        }
        std::uint32_t value = 0;
        std::uint32_t remainder = 0;
        do {
            value = nextBits(31);
            remainder = value % static_cast<std::uint32_t>(bound);
        } while (value - remainder + static_cast<std::uint32_t>(bound - 1) >= 0x80000000U);
        return static_cast<int>(remainder);
    }

    float nextFloat() { return static_cast<float>(nextBits(24)) * 0x1p-24F; }

    double nextDouble() {
        const std::uint64_t high = nextBits(26);
        const std::uint64_t low = nextBits(27);
        return static_cast<double>((high << 27) + low) * 0x1p-53;
    }

    bool nextBoolean() { return nextBits(1) != 0; }

    // Java's `nextLong` sign-extends the low half before adding it, so the two
    // halves are not independent: the low draw can borrow from the high one.
    std::uint64_t nextLong() {
        const std::uint64_t high = nextBits(32);
        const auto low = static_cast<std::int64_t>(static_cast<std::int32_t>(nextBits(32)));
        return (high << 32) + static_cast<std::uint64_t>(low);
    }

private:
    // Deliberately not named `bits`: a derived source that forgets to define
    // its own must fail to compile, not recurse into this one.
    std::uint32_t nextBits(int count) { return static_cast<Source&>(*this).bits(count); }
};

// Java: `LegacyRandomSource`, i.e. `java.util.Random`'s 48-bit LCG.
class LegacyRandom : public RandomDraws<LegacyRandom> {
public:
    explicit LegacyRandom(std::uint64_t seed) { setSeed(seed); }

    void setSeed(std::uint64_t seed) { state_ = (seed ^ kMultiplier) & kMask; }

    // Top `count` bits of the next LCG state, the primitive all draws build on.
    std::uint32_t bits(int count) {
        state_ = (state_ * kMultiplier + kIncrement) & kMask;
        return static_cast<std::uint32_t>(state_ >> (48 - count));
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5deece66dULL;
    static constexpr std::uint64_t kIncrement = 11;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;

    std::uint64_t state_{};
};

// Xoroshiro-backed WorldgenRandom used by 26.3 biome decoration. Minecraft's
// wrapper intentionally applies java.util.Random's bit-based draw algorithms
// to the high bits of one fresh xoroshiro long per `bits()` call.
class WorldgenRandom : public RandomDraws<WorldgenRandom> {
public:
    explicit WorldgenRandom(std::uint64_t seed) { setSeed(seed); }

    void setSeed(std::uint64_t seed) {
        constexpr std::uint64_t silver = 7640891576956012809ULL;
        constexpr std::uint64_t golden = 0x9e3779b97f4a7c15ULL;
        const std::uint64_t low = seed ^ silver;
        seedLo_ = mixStafford13(low);
        seedHi_ = mixStafford13(low + golden);
        if ((seedLo_ | seedHi_) == 0) {
            seedLo_ = golden;
            seedHi_ = silver;
        }
    }

    std::uint32_t bits(int count) {
        return static_cast<std::uint32_t>(nextXoroshiroLong() >> (64 - count));
    }

    // Java: `setDecorationSeed`. One seed per chunk, mixing the world seed with
    // the chunk's origin, that every feature in that chunk reseeds from.
    std::uint64_t setDecorationSeed(std::int64_t seed, int blockX, int blockZ) {
        setSeed(static_cast<std::uint64_t>(seed));
        const std::uint64_t xScale = nextLong() | 1ULL;
        const std::uint64_t zScale = nextLong() | 1ULL;
        const std::uint64_t result = (static_cast<std::uint64_t>(blockX) * xScale
            + static_cast<std::uint64_t>(blockZ) * zScale)
            ^ static_cast<std::uint64_t>(seed);
        setSeed(result);
        return result;
    }

    // Java: `setFeatureSeed`. A feature's index within its step and the step's
    // own ordinal both salt the seed, so features cannot shift each other.
    void setFeatureSeed(std::uint64_t decorationSeed, int index, int step) {
        setSeed(decorationSeed + static_cast<std::uint64_t>(index)
                + static_cast<std::uint64_t>(10000 * step));
    }

private:
    std::uint64_t nextXoroshiroLong() {
        const std::uint64_t first = seedLo_;
        std::uint64_t second = seedHi_;
        const std::uint64_t result = std::rotl(first + second, 17) + first;
        second ^= first;
        seedLo_ = std::rotl(first, 49) ^ second ^ (second << 21);
        seedHi_ = std::rotl(second, 28);
        return result;
    }

    std::uint64_t seedLo_{};
    std::uint64_t seedHi_{};
};

// Java: `WorldgenRandom.setLargeFeatureSeed`. Two draws off the world seed
// scale the chunk coordinates, so a chunk's stream depends on its position
// rather than on how many chunks were generated before it.
[[nodiscard]] inline LegacyRandom largeFeatureRandom(std::int64_t seed, int chunkX, int chunkZ) {
    LegacyRandom random(static_cast<std::uint64_t>(seed));
    const std::uint64_t xScale = random.nextLong();
    const std::uint64_t zScale = random.nextLong();
    random.setSeed(static_cast<std::uint64_t>(chunkX) * xScale
                   ^ static_cast<std::uint64_t>(chunkZ) * zScale
                   ^ static_cast<std::uint64_t>(seed));
    return random;
}

// Java: `WorldgenRandom.setLargeFeatureWithSalt`. The salt separates the
// placement grids of different structure sets that share a seed; the two large
// primes are Minecraft's own.
[[nodiscard]] inline LegacyRandom largeFeatureWithSaltRandom(
    std::int64_t seed,
    int regionX,
    int regionZ,
    int salt
) {
    const std::uint64_t mixed = static_cast<std::uint64_t>(regionX) * 341873128712ULL
        + static_cast<std::uint64_t>(regionZ) * 132897987541ULL
        + static_cast<std::uint64_t>(seed) + static_cast<std::uint32_t>(salt);
    return LegacyRandom(mixed);
}

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
    return mixStafford13(
        deriveSeed(worldSeed, key)
        ^ mixStafford13(static_cast<std::uint64_t>(x))
        ^ mixStafford13(static_cast<std::uint64_t>(y) + kYSalt)
        ^ mixStafford13(static_cast<std::uint64_t>(z) + kZSalt));
}

} // namespace mcworld::detail

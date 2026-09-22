// Biome lookups and biome-derived environment, as the terrain passes need them
// (step 6 of `minecraft-26.3-worldgen.dot`, seen from `7B`).
//
// The chunk's biome palette is stored per 4x4x4 quart cell, but material rules
// and carver repair ask per block. Java bridges that with `BiomeManager`, which
// jitters the quart lookup so biome borders are ragged instead of grid-aligned,
// and with `Biome.TemperatureModifier.FROZEN`, which decides where icebergs
// melt. Both are reproduced here; both are seeded independently of the density
// noise, so their constants are not interchangeable with the router's.

#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <utility>

namespace mcworld::detail {
namespace {

// --- SHA-256 over a single seed --------------------------------------------

constexpr std::uint32_t kSha256RoundConstants[64]{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t kSha256InitialState[8]{
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// First two state words of SHA-256 over the eight little-endian bytes of
// `seed`. The message is short enough that padding always fits in one block,
// which keeps this dependency-free: byte 8 carries the padding bit and the
// final word carries the 64-bit length.
[[nodiscard]] std::array<std::uint32_t, 2> sha256PrefixOfSeed(std::int64_t seed) {
    std::array<std::uint32_t, 64> schedule{};
    for (int byte = 0; byte < 8; ++byte) {
        const auto value = static_cast<std::uint32_t>((static_cast<std::uint64_t>(seed) >> (8 * byte)) & 0xFF);
        schedule[byte / 4] |= value << (24 - 8 * (byte % 4));
    }
    schedule[2] = 0x80000000; // Padding: a single one bit after the message.
    schedule[15] = 64;        // Message length in bits.
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t a = schedule[i - 15];
        const std::uint32_t b = schedule[i - 2];
        schedule[i] = schedule[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + schedule[i - 7] +
                      (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
    }

    std::uint32_t a = kSha256InitialState[0];
    std::uint32_t b = kSha256InitialState[1];
    std::uint32_t c = kSha256InitialState[2];
    std::uint32_t d = kSha256InitialState[3];
    std::uint32_t e = kSha256InitialState[4];
    std::uint32_t f = kSha256InitialState[5];
    std::uint32_t g = kSha256InitialState[6];
    std::uint32_t h = kSha256InitialState[7];
    for (int round = 0; round < 64; ++round) {
        const std::uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                                 kSha256RoundConstants[round] + schedule[round];
        const std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    return {a + kSha256InitialState[0], b + kSha256InitialState[1]};
}

// --- Jittered quart selection ----------------------------------------------

// Quart Y range covered by a chunk's biome palette, in quart units.
constexpr int kMinPaletteQuartY = TerrainChunk::minY / kQuartSize;
constexpr int kMaxPaletteQuartY = TerrainChunk::maxY / kQuartSize;

// Java: `BiomeManager` mixes coordinates into the seed with this LCG step.
[[nodiscard]] std::uint64_t mixSeed(std::uint64_t seed, std::uint64_t value) {
    return seed * (seed * 6364136223846793005ULL + 1442695040888963407ULL) + value;
}

// Java: `BiomeManager.getFiddle`. Ten bits out of the middle of the state,
// mapped to roughly +/-0.45 of a quart cell.
[[nodiscard]] double fiddle(std::uint64_t seed) {
    return (static_cast<double>((seed >> 24) & 1023) / 1024.0 - 0.5) * 0.9;
}

} // namespace

std::uint64_t biomeZoomSeed(std::int64_t seed) {
    // Java: `BiomeManager.obfuscateSeed` reads the first eight digest bytes
    // back as a little-endian long.
    const auto digest = sha256PrefixOfSeed(seed);
    std::uint64_t result = 0;
    for (int byte = 0; byte < 8; ++byte) {
        const std::uint32_t word = digest[byte < 4 ? 0 : 1];
        result |= static_cast<std::uint64_t>((word >> (24 - 8 * (byte % 4))) & 0xFF) << (byte * 8);
    }
    return result;
}

BlockPosition zoomedBiomeQuart(std::uint64_t seed, int x, int y, int z) {
    // The lookup is offset by two blocks so that a block sits between quart
    // centers rather than on one. Generator coordinate validation reserves
    // enough headroom for that shift.
    constexpr int kLookupOffset = 2;
    const int baseX = floorDiv(x - kLookupOffset, kQuartSize);
    const int baseY = floorDiv(y - kLookupOffset, kQuartSize);
    const int baseZ = floorDiv(z - kLookupOffset, kQuartSize);
    const double fractionX = (x - kLookupOffset - baseX * kQuartSize) / static_cast<double>(kQuartSize);
    const double fractionY = (y - kLookupOffset - baseY * kQuartSize) / static_cast<double>(kQuartSize);
    const double fractionZ = (z - kLookupOffset - baseZ * kQuartSize) / static_cast<double>(kQuartSize);

    // Of the eight surrounding quart cells, take the one whose jittered center
    // is nearest. Ties keep the earlier corner, so the iteration order matters.
    double best = std::numeric_limits<double>::infinity();
    BlockPosition result;
    for (int corner = 0; corner < 8; ++corner) {
        const int stepX = (corner >> 2) & 1;
        const int stepY = (corner >> 1) & 1;
        const int stepZ = corner & 1;
        const int cellX = baseX + stepX;
        const int cellY = baseY + stepY;
        const int cellZ = baseZ + stepZ;

        std::uint64_t state = seed;
        for (int coordinate : {cellX, cellY, cellZ, cellX, cellY, cellZ}) {
            state = mixSeed(state, static_cast<std::uint64_t>(coordinate));
        }
        const double dx = fractionX - stepX + fiddle(state);
        state = mixSeed(state, seed);
        const double dy = fractionY - stepY + fiddle(state);
        state = mixSeed(state, seed);
        const double dz = fractionZ - stepZ + fiddle(state);

        const double distance = dz * dz + dy * dy + dx * dx;
        if (distance < best) {
            best = distance;
            result = {cellX, cellY, cellZ};
        }
    }
    return result;
}

BlockBiomeGetter makeBlockBiomeGetter(
    const TerrainChunk& chunk,
    const OverworldNoiseRouter& router,
    const BiomeSource& source,
    bool clampY
) {
    return [&chunk, &router, &source, clampY, seed = biomeZoomSeed(router.seed()),
            cache = std::map<std::tuple<int, int, int>, Biome>{}](int x, int y, int z) mutable {
        BlockPosition cell = zoomedBiomeQuart(seed, x, y, z);
        if (clampY) {
            // Java: `ChunkAccess.getNoiseBiome` clamps into the palette.
            cell.y = std::clamp(cell.y, kMinPaletteQuartY, kMaxPaletteQuartY - 1);
        }
        const int blockX = cell.x * kQuartSize;
        const int blockZ = cell.z * kQuartSize;
        const int localX = blockX - chunk.chunkX * TerrainChunk::width;
        const int localZ = blockZ - chunk.chunkZ * TerrainChunk::width;
        const bool insideChunk = localX >= 0 && localX < TerrainChunk::width && localZ >= 0 &&
                                 localZ < TerrainChunk::width && cell.y >= kMinPaletteQuartY &&
                                 cell.y < kMaxPaletteQuartY;
        if (insideChunk) {
            return chunk.biomeAt(localX, cell.y * kQuartSize, localZ);
        }
        // Outside the chunk the biome source answers directly; memoize, because
        // neighbouring columns keep landing in the same quart cell.
        auto [entry, inserted] = cache.try_emplace({cell.x, cell.y, cell.z});
        if (inserted) {
            entry->second = source.sample(router, blockX, cell.y * kQuartSize, blockZ);
        }
        return entry->second;
    };
}

namespace {

// --- Frozen-ocean temperature ----------------------------------------------

// Java: `SimplexNoise`. The frozen-ocean temperature modifier runs on its own
// fixed-seed, offset-free 2-D simplex stack, independent of the world's
// density-noise seed convention.
class TemperatureSimplex {
public:
    explicit TemperatureSimplex(LegacyRandom& random) {
        // Java samples three offsets it then never uses for the 2-D case; the
        // draws still have to happen or the permutation shuffle shifts.
        for (int i = 0; i < 3; ++i) {
            (void)random.nextDouble();
        }
        std::iota(permutation_.begin(), permutation_.end(), 0);
        for (int i = 0; i < kPermutationSize; ++i) {
            const int j = i + random.nextInt(kPermutationSize - i);
            std::swap(permutation_[i], permutation_[j]);
        }
    }

    [[nodiscard]] float sample(double x, double z) const {
        static const double skew = (std::sqrt(3.0) - 1) * 0.5;
        static const double unskew = (3 - std::sqrt(3.0)) / 6;

        // Skew into the simplex lattice and find the containing cell.
        const double skewed = (x + z) * skew;
        const int cellX = static_cast<int>(std::floor(x + skewed));
        const int cellZ = static_cast<int>(std::floor(z + skewed));
        const double unskewed = (cellX + cellZ) * unskew;
        const double dx = x - (cellX - unskewed);
        const double dz = z - (cellZ - unskewed);
        // Which of the two triangles in the cell the point falls into.
        const int stepX = dx > dz ? 1 : 0;
        const int stepZ = 1 - stepX;

        const double first = corner(gradientIndex(cellX, cellZ, 0, 0), dx, dz);
        const double second = corner(gradientIndex(cellX, cellZ, stepX, stepZ), dx - stepX + unskew, dz - stepZ + unskew);
        const double third = corner(gradientIndex(cellX, cellZ, 1, 1), dx - 1 + 2 * unskew, dz - 1 + 2 * unskew);
        return static_cast<float>(70 * (first + second + third));
    }

private:
    static constexpr int kPermutationSize = 256;

    [[nodiscard]] int permuted(int value) const { return permutation_[value & (kPermutationSize - 1)]; }

    [[nodiscard]] int gradientIndex(int cellX, int cellZ, int stepX, int stepZ) const {
        return permuted((cellX & 255) + stepX + permuted((cellZ & 255) + stepZ)) % 12;
    }

    // Radially attenuated gradient contribution of one simplex corner.
    [[nodiscard]] static double corner(int gradient, double a, double b) {
        // The 3-D gradient table, projected onto XZ as Java does.
        constexpr int kGradients[][2]{{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0},
                                      {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 1}, {0, -1}};
        double attenuation = 0.5 - a * a - b * b;
        if (attenuation < 0) {
            return 0.0;
        }
        attenuation *= attenuation;
        return attenuation * attenuation * (kGradients[gradient][0] * a + kGradients[gradient][1] * b);
    }

    std::array<int, kPermutationSize> permutation_;
};

} // namespace

bool meltsFrozenOceanIceberg(Biome biome, int x, int z) {
    // Deep frozen oceans sit at base temperature 0.5, or 0.2 once modified;
    // both are above the 0.1 melting threshold, so the noise is irrelevant.
    if (biome == Biome::DeepFrozenOcean) {
        return true;
    }

    // Java: `Biome.getTemperature` with `TemperatureModifier.FROZEN`, built on
    // two fixed-seed noise stacks.
    static const auto frozenness = [] {
        LegacyRandom random(3456);
        return std::array{TemperatureSimplex(random), TemperatureSimplex(random), TemperatureSimplex(random)};
    }();
    static const auto detail = [] {
        LegacyRandom random(2345);
        return TemperatureSimplex(random);
    }();

    constexpr double kFrequencies[]{1, .5, .25};
    constexpr float kAmplitudes[]{.14285715F, .2857143F, .5714286F};
    float large = 0;
    for (int octave = 0; octave < 3; ++octave) {
        large += kAmplitudes[octave] * frozenness[octave].sample(x * .05 * kFrequencies[octave], z * .05 * kFrequencies[octave]);
    }
    return static_cast<double>(large * 7.0F) + detail.sample(x * .2, z * .2) < .3 && detail.sample(x * .09, z * .09) < .8;
}

} // namespace mcworld::detail

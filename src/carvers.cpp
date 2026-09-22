// Carvers: the `carver_scan` .. `apply_mask` nodes of `7B` in
// `minecraft-26.3-worldgen.dot`.
//
// Java: `CaveWorldCarver` and `CanyonWorldCarver`. Every chunk within radius 8
// of the one being generated gets a chance to start a cave system or a canyon,
// and the geometry those random walks sweep out is recorded in a bit mask.
// Carving therefore depends only on the world seed and the *source* chunk
// position, never on generation order.
//
// The mask is geometry only. `applyCarvingMask` decides what actually happens
// to a marked block: the aquifer supplies air, water or lava, bedrock is never
// removed, and soil left exposed under grass or mycelium is repaired.

#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <utility>

namespace mcworld::detail {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// --- Trigonometry ----------------------------------------------------------

// Java: `Mth.SIN`. Minecraft walks its carvers with a 65,536-entry sine table
// rather than `Math.sin`, so the walk cannot drift with the host libm. Building
// the table is still subject to libm rounding; see `WORLDGEN_AUDIT.md`.
constexpr int kTrigTableSize = 65536;
constexpr float kRadiansToTableIndex = 10430.378F; // 65536 / (2 * pi)
constexpr float kQuarterTurn = kTrigTableSize / 4;

const std::array<float, kTrigTableSize>& trigTable() {
    static const auto table = [] {
        std::array<float, kTrigTableSize> values{};
        for (int i = 0; i < kTrigTableSize; ++i) {
            values[i] = static_cast<float>(std::sin(i * 2.0 * std::numbers::pi / kTrigTableSize));
        }
        return values;
    }();
    return table;
}

[[nodiscard]] float sine(float angle) {
    return trigTable()[static_cast<int>(angle * kRadiansToTableIndex) & (kTrigTableSize - 1)];
}

[[nodiscard]] float cosine(float angle) {
    return trigTable()[static_cast<int>(angle * kRadiansToTableIndex + kQuarterTurn) & (kTrigTableSize - 1)];
}

// --- Random helpers --------------------------------------------------------
//
// Each of these consumes a fixed number of draws in a fixed order. That count
// is part of the stream: taking one draw fewer shifts every later decision of
// the same carver.

[[nodiscard]] float uniform(LegacyRandom& random, float low, float high) {
    return low + random.nextFloat() * (high - low);
}

// Two stacked uniforms: a triangular distribution peaked at 1.5 * `scale`.
[[nodiscard]] float trapezoid(LegacyRandom& random, float scale) {
    const float wide = random.nextFloat();
    const float narrow = random.nextFloat();
    return wide * (scale * 2) + narrow * scale;
}

// Symmetric drift term used to steer the random walks.
[[nodiscard]] float perturb(LegacyRandom& random, float scale) {
    const float first = random.nextFloat();
    const float second = random.nextFloat();
    const float magnitude = random.nextFloat();
    return (first - second) * magnitude * scale;
}

// --- Mask ------------------------------------------------------------------

// The carving mask of one chunk, plus the geometry that writes into it.
//
// Positions outside the chunk are simply ignored, which is how a cave system
// started eight chunks away contributes only its overlapping part.
class Mask {
public:
    Mask(int chunkX, int chunkZ) : chunkX_(chunkX), chunkZ_(chunkZ) {}

    // Hands over the finished mask; the mask object is spent afterwards.
    [[nodiscard]] CarvingMask release() { return std::move(bits_); }

    // Java: cave carvers. A handful of tunnel systems, optionally preceded by a
    // room, starting from a random block of the source chunk.
    void addCaves(LegacyRandom& random, int sourceX, int sourceZ, bool extraUnderground) {
        // Java's nested draws: most source chunks get very few cave systems.
        const int systems = random.nextInt(random.nextInt(random.nextInt(15) + 1) + 1);
        for (int system = 0; system < systems; ++system) {
            const double x = sourceX * TerrainChunk::width + random.nextInt(TerrainChunk::width);
            const double y = -56 + random.nextInt(extraUnderground ? 104 : 237);
            const double z = sourceZ * TerrainChunk::width + random.nextInt(TerrainChunk::width);
            const double horizontalScale = uniform(random, .7F, 1.4F);
            const double verticalScale = uniform(random, .8F, 1.3F);
            const double floor = uniform(random, -1, -.4F);

            int tunnels = 1;
            if (random.nextInt(4) == 0) {
                // A wide room at the start, and extra tunnels leading out of it.
                const double roomFlatness = uniform(random, .1F, .9F);
                const float thickness = 1 + random.nextFloat() * 6;
                carveSphere(x + 1, y, z, 1.5 + thickness, (1.5 + thickness) * roomFlatness, floor);
                tunnels += random.nextInt(4);
            }
            for (int tunnel = 0; tunnel < tunnels; ++tunnel) {
                const float yaw = random.nextFloat() * (kPi * 2);
                const float pitch = (random.nextFloat() - .5F) / 4;
                float thickness = trapezoid(random, 1);
                if (random.nextInt(10) == 0) {
                    // One tunnel in ten is a notably larger one.
                    const float first = random.nextFloat();
                    const float second = random.nextFloat();
                    thickness *= first * second * 3 + 1;
                }
                const int steps = 112 - random.nextInt(28);
                const std::uint64_t walkSeed = random.nextLong();
                walkTunnel(walkSeed, x, y, z, horizontalScale, verticalScale, thickness, yaw, pitch, 0, steps, floor);
            }
        }
    }

    // Java: `CanyonWorldCarver`. One long, narrow, steep-walled walk whose
    // width varies per Y level.
    void addCanyon(LegacyRandom& source, int sourceX, int sourceZ) {
        double x = sourceX * TerrainChunk::width + source.nextInt(TerrainChunk::width);
        double y = 10 + source.nextInt(58);
        double z = sourceZ * TerrainChunk::width + source.nextInt(TerrainChunk::width);
        float yaw = source.nextFloat() * (kPi * 2);
        float pitch = uniform(source, -.125F, .125F);
        const float thickness = trapezoid(source, 2);
        const int steps = static_cast<int>(112 * uniform(source, .75F, 1));

        // The walk continues on its own stream, seeded from the source stream.
        LegacyRandom random(source.nextLong());
        const auto widths = canyonWidths(random);

        float yawDrift = 0;
        float pitchDrift = 0;
        for (int step = 0; step < steps; ++step) {
            double horizontal = 1.5 + sine(step * kPi / steps) * thickness;
            double vertical = horizontal * 3;
            horizontal *= uniform(random, .75F, 1);
            vertical *= uniform(random, .75F, 1);

            const float pitchCosine = cosine(pitch);
            x += cosine(yaw) * pitchCosine;
            y += sine(pitch);
            z += sine(yaw) * pitchCosine;
            pitch *= .7F;
            pitch += pitchDrift * .05F;
            yaw += yawDrift * .05F;
            pitchDrift *= .8F;
            yawDrift *= .5F;
            pitchDrift += perturb(random, 2);
            yawDrift += perturb(random, 4);

            if (random.nextInt(4) != 0) {
                if (!reachable(x, z, step, steps, thickness)) {
                    return;
                }
                carveCanyonSlice(x, y, z, horizontal, vertical, widths);
            }
        }
    }

private:
    // Mask bounds. Java protects the bottom of the world and the top seven
    // blocks, and `WorldCarver`'s loop excludes the lower bound itself, so
    // geometry can mark [-62, 312].
    static constexpr int kMinCarvedY = -63;
    static constexpr int kMaxCarvedY = 312;

    // Per-Y squared width multipliers of a canyon, indexed from `kMinCarvedY`.
    using CanyonWidths = std::array<float, TerrainChunk::height>;

    [[nodiscard]] static CanyonWidths canyonWidths(LegacyRandom& random) {
        CanyonWidths widths{};
        float width = 1;
        for (std::size_t y = 0; y < widths.size(); ++y) {
            if (y == 0 || random.nextInt(3) == 0) {
                const float first = random.nextFloat();
                const float second = random.nextFloat();
                width = 1 + first * second;
            }
            widths[y] = width * width;
        }
        return widths;
    }

    // Whether the walk can still reach this chunk in the steps it has left. A
    // walk that cannot is abandoned, which is what bounds the work per chunk.
    [[nodiscard]] bool reachable(double x, double z, int step, int steps, float width) const {
        const double dx = x - (chunkX_ * TerrainChunk::width + 8);
        const double dz = z - (chunkZ_ * TerrainChunk::width + 8);
        const double remaining = steps - step;
        const double radius = width + 2.0F + 16.0F;
        return dx * dx + dz * dz - remaining * remaining <= radius * radius;
    }

    // Marks an axis-aligned ellipsoid, flattened below `floor` so caves get a
    // walkable bottom instead of a round one.
    void carveSphere(double x, double y, double z, double horizontal, double vertical, double floor) {
        forEachCandidate(x, y, z, horizontal, vertical, [&](double dx, double dy, double dz, int) {
            return dy > floor && dx * dx + dy * dy + dz * dz < 1;
        });
    }

    // Marks one slice of a canyon: same ellipsoid, but scaled per Y level and
    // stretched vertically, which is what makes canyons tall and ragged.
    void carveCanyonSlice(double x, double y, double z, double horizontal, double vertical, const CanyonWidths& widths) {
        forEachCandidate(x, y, z, horizontal, vertical, [&](double dx, double dy, double dz, int blockY) {
            return (dx * dx + dz * dz) * widths[blockY - kMinCarvedY] + dy * dy / 6 < 1;
        });
    }

    // Walks the blocks that could fall inside an ellipsoid of the given size,
    // in X, Z, descending-Y order, and marks the ones `inside` accepts. The
    // deltas handed to `inside` are already normalized by the radii.
    template <typename Inside>
    void forEachCandidate(double x, double y, double z, double horizontal, double vertical, Inside inside) {
        const int originX = chunkX_ * TerrainChunk::width;
        const int originZ = chunkZ_ * TerrainChunk::width;

        // Cheap rejection for geometry from a distant source chunk.
        const double maxDelta = 16 + horizontal * 2;
        if (std::abs(x - (originX + 8)) > maxDelta || std::abs(z - (originZ + 8)) > maxDelta) {
            return;
        }

        const int minX = std::max(0, static_cast<int>(std::floor(x - horizontal)) - originX - 1);
        const int maxX = std::min(TerrainChunk::width - 1, static_cast<int>(std::floor(x + horizontal)) - originX);
        const int minZ = std::max(0, static_cast<int>(std::floor(z - horizontal)) - originZ - 1);
        const int maxZ = std::min(TerrainChunk::width - 1, static_cast<int>(std::floor(z + horizontal)) - originZ);
        const int minY = std::max(kMinCarvedY, static_cast<int>(std::floor(y - vertical)) - 1);
        const int maxY = std::min(kMaxCarvedY, static_cast<int>(std::floor(y + vertical)) + 1);

        for (int blockX = minX; blockX <= maxX; ++blockX) {
            for (int blockZ = minZ; blockZ <= maxZ; ++blockZ) {
                const double dx = (originX + blockX + .5 - x) / horizontal;
                const double dz = (originZ + blockZ + .5 - z) / horizontal;
                if (dx * dx + dz * dz >= 1) {
                    continue;
                }
                // The lower bound is excluded, matching Java's loop.
                for (int blockY = maxY; blockY > minY; --blockY) {
                    const double dy = (blockY - .5 - y) / vertical;
                    if (inside(dx, dy, dz, blockY)) {
                        bits_[carvingMaskIndex(blockX, blockY, blockZ)] = true;
                    }
                }
            }
        }
    }

    // One tunnel of a cave system: a random walk that sweeps a sphere along its
    // path, and that can fork once into two thinner branches.
    void walkTunnel(
        std::uint64_t seed,
        double x,
        double y,
        double z,
        double horizontalScale,
        double verticalScale,
        float thickness,
        float yaw,
        float pitch,
        int firstStep,
        int steps,
        double floor
    ) {
        LegacyRandom random(seed);
        const int forkStep = random.nextInt(steps / 2) + steps / 4;
        const bool steep = random.nextInt(6) == 0;

        float yawDrift = 0;
        float pitchDrift = 0;
        for (int step = firstStep; step < steps; ++step) {
            // Tunnels are widest in the middle of their run.
            const double horizontal = 1.5 + sine(kPi * step / steps) * thickness;
            const double vertical = horizontal; // Cave tunnels are round in cross-section.

            const float pitchCosine = cosine(pitch);
            x += cosine(yaw) * pitchCosine;
            y += sine(pitch);
            z += sine(yaw) * pitchCosine;
            pitch *= steep ? .92F : .7F;
            pitch += pitchDrift * .1F;
            yaw += yawDrift * .1F;
            pitchDrift *= .9F;
            yawDrift *= .75F;
            pitchDrift += perturb(random, 2);
            yawDrift += perturb(random, 4);

            if (step == forkStep && thickness > 1) {
                // Fork left and right; both branches continue from here, and
                // this walk stops.
                const std::uint64_t leftSeed = random.nextLong();
                const float leftThickness = random.nextFloat() * .5F + .5F;
                walkTunnel(leftSeed, x, y, z, horizontalScale, verticalScale, leftThickness,
                           yaw - kPi / 2, pitch / 3, step, steps, floor);
                const std::uint64_t rightSeed = random.nextLong();
                const float rightThickness = random.nextFloat() * .5F + .5F;
                walkTunnel(rightSeed, x, y, z, horizontalScale, verticalScale, rightThickness,
                           yaw + kPi / 2, pitch / 3, step, steps, floor);
                return;
            }

            // Three steps in four carve; the gaps are what make tunnels pinch.
            if (random.nextInt(4) != 0) {
                if (!reachable(x, z, step, steps, thickness)) {
                    return;
                }
                carveSphere(x, y, z, horizontal * horizontalScale, vertical * verticalScale, floor);
            }
        }
    }

    int chunkX_;
    int chunkZ_;
    CarvingMask bits_ = CarvingMask(TerrainChunk::width * TerrainChunk::width * TerrainChunk::height);
};

// Carves one vertical run, top-down. Tracking grass or mycelium on the way
// down is what lets exposed dirt below it be repaired into topsoil; the flag
// resets per run.
void carveRun(
    TerrainChunk& chunk,
    Aquifer& aquifer,
    const TopMaterialRule& topMaterial,
    int x,
    int z,
    int bottom,
    int top
) {
    bool underSurface = false;
    for (int y = top; y >= bottom; --y) {
        const Block existing = chunk.at(x, y, z);
        if (existing == Block::Bedrock) {
            continue; // Java: the UNCARVABLE tag.
        }
        if (existing == Block::Grass || existing == Block::Mycelium) {
            underSurface = true;
        }

        // Density zero: the block is open, so the aquifer decides what fills it.
        const Substance substance = aquifer.sample(chunk.chunkX * TerrainChunk::width + x, y,
                                                   chunk.chunkZ * TerrainChunk::width + z, 0);
        if (substance.block == Block::Stone) {
            continue; // Pressure kept this position solid.
        }
        setWorldgenBlock(chunk, x, y, z, substance.block);
        if (substance.schedule && isFluid(substance.block)) {
            chunk.fluidPostProcessing.push_back({x, y, z});
        }

        if (underSurface && y > TerrainChunk::minY && chunk.at(x, y - 1, z) == Block::Dirt) {
            if (const auto material = topMaterial(x, y - 1, z, isFluid(substance.block))) {
                setWorldgenBlock(chunk, x, y - 1, z, *material);
                if (isFluid(*material)) {
                    chunk.fluidPostProcessing.push_back({x, y - 1, z});
                }
            }
        }
    }
}

// The three carvers every standard Overworld biome registers, in the order
// their seeds are derived from: a carver's index is part of its seed, so the
// order of this table is part of the world.
enum class Carver { Caves, ExtraUndergroundCaves, Canyon };

struct CarverEntry {
    Carver carver;
    float probability; // Chance that a source chunk starts this carver.
};

constexpr CarverEntry kCarvers[]{
    {Carver::Caves, .15F},
    {Carver::ExtraUndergroundCaves, .07F},
    {Carver::Canyon, .01F}};

// Java: `WorldgenRandom.setLargeFeatureSeed`. Ties a carver's stream to the
// world seed and the source chunk, so neighbours agree on shared geometry.
[[nodiscard]] LegacyRandom largeFeatureRandom(std::uint64_t seed, int sourceX, int sourceZ) {
    LegacyRandom seedRandom(seed);
    const std::uint64_t saltX = seedRandom.nextLong();
    const std::uint64_t saltZ = seedRandom.nextLong();
    return LegacyRandom((static_cast<std::uint64_t>(sourceX) * saltX) ^ (static_cast<std::uint64_t>(sourceZ) * saltZ) ^ seed);
}

} // namespace

CarvingMask buildCarvingMask(int chunkX, int chunkZ, std::int64_t worldSeed) {
    constexpr int kSourceRadius = 8;
    Mask mask(chunkX, chunkZ);
    for (int sourceX = chunkX - kSourceRadius; sourceX <= chunkX + kSourceRadius; ++sourceX) {
        for (int sourceZ = chunkZ - kSourceRadius; sourceZ <= chunkZ + kSourceRadius; ++sourceZ) {
            // All standard Overworld biomes register these same three carvers,
            // so the source biome does not have to be resolved here.
            for (std::size_t index = 0; index < std::size(kCarvers); ++index) {
                const CarverEntry& entry = kCarvers[index];
                const auto seed = static_cast<std::uint64_t>(worldSeed) + index;
                LegacyRandom random = largeFeatureRandom(seed, sourceX, sourceZ);
                if (random.nextFloat() > entry.probability) {
                    continue;
                }
                switch (entry.carver) {
                case Carver::Caves:
                    mask.addCaves(random, sourceX, sourceZ, false);
                    break;
                case Carver::ExtraUndergroundCaves:
                    mask.addCaves(random, sourceX, sourceZ, true);
                    break;
                case Carver::Canyon:
                    mask.addCanyon(random, sourceX, sourceZ);
                    break;
                }
            }
        }
    }
    return mask.release();
}

void applyCarvingMask(TerrainChunk& chunk, const CarvingMask& mask, Aquifer& aquifer, const TopMaterialRule& topMaterial) {
    // Java: `CarvingMask.visit` walks X/Z columns and bottom-to-top runs, then
    // visits each run top-down. Live heightmaps make that order observable
    // through `topMaterial`, so it is part of the output.
    for (int x = 0; x < TerrainChunk::width; ++x) {
        for (int z = 0; z < TerrainChunk::width; ++z) {
            auto carved = [&](int y) { return mask[carvingMaskIndex(x, y, z)]; };
            for (int y = TerrainChunk::minY; y < TerrainChunk::maxY;) {
                if (!carved(y)) {
                    ++y;
                    continue;
                }
                const int runBottom = y;
                while (y < TerrainChunk::maxY && carved(y)) {
                    ++y;
                }
                carveRun(chunk, aquifer, topMaterial, x, z, runBottom, y - 1);
            }
        }
    }
}

void carve(TerrainChunk& chunk, const OverworldNoiseRouter& router, const BiomeSource& biomes, Aquifer& aquifer, bool veins) {
    // The biome source selects palette entries, not custom generation settings.
    // Every supported Overworld biome registers the same carver list
    // (`BiomeDefaultFeatures`), so the mask does not depend on the biomes; the
    // biomes only matter for repairing soil the carvers expose.
    const CarvingMask mask = buildCarvingMask(chunk.chunkX, chunk.chunkZ, router.seed());
    const TopMaterialRule topMaterial =
        makeTopMaterialRule(chunk, router, veins, makeBlockBiomeGetter(chunk, router, biomes, false));
    applyCarvingMask(chunk, mask, aquifer, topMaterial);
}

} // namespace mcworld::detail

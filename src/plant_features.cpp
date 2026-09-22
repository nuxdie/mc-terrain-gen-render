// Source-backed village patches and their fixed-seed flower state provider.
#include "feature_placement.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <numeric>

namespace mcworld::detail {
namespace {

// Kept separate from the terrain noise's project-specific seed convention.
// This is the one-octave, legacy-factory Perlin used by flower_plain in 26.3.
class FlowerPerlin {
public:
    explicit FlowerPerlin(std::uint64_t seed) {
        LegacyRandom random(seed);
        for (auto& offset : offsets_) offset = random.nextDouble() * 256.0;
        std::iota(permutation_.begin(), permutation_.end(), 0);
        for (int i = 0; i < 256; ++i) {
            const int j = i + random.nextInt(256 - i);
            std::swap(permutation_[i], permutation_[j]);
        }
    }

    float sample(double x, double y, double z) const {
        const auto wrap = [](double v) { return v - std::floor(v / 33554432.0 + .5) * 33554432.0; };
        x = wrap(x) + offsets_[0]; y = wrap(y) + offsets_[1]; z = wrap(z) + offsets_[2];
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)), iz = static_cast<int>(std::floor(z));
        const float dx = static_cast<float>(x - ix), dy = static_cast<float>(y - iy), dz = static_cast<float>(z - iz);
        const auto fade = [](float v) { return v * v * v * (v * (v * 6.0F - 15.0F) + 10.0F); };
        const auto lerp = [](float a, float low, float high) { return low + a * (high - low); };
        const float ax = fade(dx), ay = fade(dy), az = fade(dz);
        std::array<float, 8> values{};
        for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
            const int hash = permute(permute(permute(ix + x) + iy + y) + iz + z);
            constexpr int gradients[16][3]{{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},
                {1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},
                {1,1,0},{0,-1,1},{-1,1,0},{0,-1,-1}};
            const auto& g = gradients[hash & 15];
            values[z * 4 + y * 2 + x] = g[0] * (dx - x) + g[1] * (dy - y) + g[2] * (dz - z);
        }
        return lerp(az, lerp(ay, lerp(ax, values[0], values[1]), lerp(ax, values[2], values[3])),
                        lerp(ay, lerp(ax, values[4], values[5]), lerp(ax, values[6], values[7])));
    }
private:
    int permute(int i) const { return permutation_[i & 255]; }
    std::array<int, 256> permutation_{};
    std::array<double, 3> offsets_{};
};

void plant(FeatureWorld& world, BlockPosition p, std::string state, Block material = Block::Plant) {
    world.set(p.x, p.y, p.z, material);
    world.setData(p.x, p.y, p.z, std::make_shared<BlockData>(BlockData{std::move(state), {}}));
}

bool vegetationSoil(Block block) {
    return block == Block::Grass || block == Block::Dirt || block == Block::CoarseDirt
        || block == Block::Podzol || block == Block::Mycelium || block == Block::Mud;
}

bool cactusSurvives(const FeatureWorld& world, BlockPosition p) {
    for (const auto& d : std::array<BlockPosition, 4>{{{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}}}) {
        const Block neighbor = world.at(p.x + d.x, p.y, p.z + d.z);
        if (blocksMotion(neighbor) || neighbor == Block::Lava) return false;
    }
    const Block below = world.at(p.x, p.y - 1, p.z);
    return (below == Block::Sand || below == Block::RedSand || below == Block::Cactus)
        && !isFluid(world.at(p.x, p.y + 1, p.z));
}

std::string flowerState(WorldgenRandom& random, BlockPosition p) {
    if (plainsFlowerNoise(p) < -.8F) {
        constexpr std::array low{"orange_tulip", "red_tulip", "pink_tulip", "white_tulip"};
        return std::string("minecraft:") + low[random.nextInt(4)];
    }
    if (random.nextFloat() < .33333334F) {
        constexpr std::array high{"poppy", "azure_bluet", "oxeye_daisy", "cornflower"};
        return std::string("minecraft:") + high[random.nextInt(4)];
    }
    return "minecraft:dandelion";
}
} // namespace

float plainsFlowerNoise(BlockPosition p) {
    static const auto noises = [] {
        LegacyRandom random(2345);
        std::uint32_t hash = 0;
        for (char c : std::string_view("octave_0")) hash = hash * 31 + static_cast<unsigned char>(c);
        const auto signedHash = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(hash)));
        const auto first = random.nextLong() ^ signedHash;
        const auto second = random.nextLong() ^ signedHash;
        return std::array{FlowerPerlin(first), FlowerPerlin(second)};
    }();
    static const float amplitude = [] {
        constexpr double base = .955388882960065;
        const double deviation = .2702247831245211 * base;
        return static_cast<float>((base * .3333333333333333 / (std::sqrt(deviation * deviation) * std::sqrt(2.0))) * base);
    }();
    constexpr double scale = static_cast<double>(.005F);
    constexpr double detune = 1.0181268882175227;
    const double x = p.x * scale, y = p.y * scale, z = p.z * scale;
    float result = 0;
    result += amplitude * noises[0].sample(x, y, z);
    result += amplitude * noises[1].sample(x * detune, y * detune, z * detune);
    return result;
}

ConfiguredFeature cactusColumnFeature() {
    return [](FeatureContext& c, BlockPosition p) {
        // BlockColumnFeature: sample every layer before probing; its lookahead
        // starts above the origin. Truncation removes the tip layer first.
        const int upper = c.random.nextInt(3);
        int stem = 1 + c.random.nextInt(upper + 1);
        int flower = c.random.nextInt(4) == 3 ? 1 : 0;
        const int total = stem + flower;
        for (int i = 0; i < total; ++i) {
            const BlockPosition above{p.x, p.y + i + 1, p.z};
            if (!c.world.canWrite(above) || c.world.at(above.x, above.y, above.z) != Block::Air) {
                int remove = total - i;
                const int tip = std::min(remove, flower);
                flower -= tip; remove -= tip; stem -= std::min(remove, stem);
                break;
            }
        }
        for (int i = 0; i < stem; ++i) if (c.world.canWrite({p.x, p.y + i, p.z}))
            plant(c.world, {p.x, p.y + i, p.z}, "minecraft:cactus[age=0]", Block::Cactus);
        if (flower && c.world.canWrite({p.x, p.y + stem, p.z}))
            plant(c.world, {p.x, p.y + stem, p.z}, "minecraft:cactus_flower");
        return true;
    };
}

PlacedFeature villagePlantPatch(VillagePlantPatch kind) {
    using namespace placement;
    const bool flowers = kind == VillagePlantPatch::PlainsFlowers;
    const int attempts = kind == VillagePlantPatch::Cactus ? 10 : kind == VillagePlantPatch::Berries ? 96 : flowers ? 64 : 32;
    ConfiguredFeature feature;
    if (kind == VillagePlantPatch::Cactus) feature = cactusColumnFeature();
    else feature = [kind](FeatureContext& c, BlockPosition p) {
        // SimpleBlockFeature samples its provider before the survival check.
        std::string state = kind == VillagePlantPatch::Berries ? "minecraft:sweet_berry_bush[age=3]"
            : kind == VillagePlantPatch::TaigaGrass ? (c.random.nextInt(5) == 0 ? "minecraft:short_grass" : "minecraft:fern")
            : flowerState(c.random, p);
        if (!vegetationSoil(c.world.at(p.x, p.y - 1, p.z))) return false;
        plant(c.world, p, std::move(state));
        return true;
    };
    const int horizontal = flowers ? 6 : 7, vertical = flowers ? 2 : 3;
    return {std::move(feature), {count(constant(attempts)), offset(triangle(-horizontal, horizontal), triangle(-vertical, vertical)),
        filter([kind](const FeatureWorld& world, BlockPosition p) {
            if (!world.canWrite(p) || world.at(p.x, p.y, p.z) != Block::Air) return false;
            if (kind == VillagePlantPatch::Berries) return world.at(p.x, p.y - 1, p.z) == Block::Grass;
            return kind != VillagePlantPatch::Cactus || cactusSurvives(world, p);
        })}};
}
} // namespace mcworld::detail

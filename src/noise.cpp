#include "noise.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace mcworld::detail {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

float fade(float value) {
    return value * value * value * (value * (value * 6.0F - 15.0F) + 10.0F);
}

float lerp(float amount, float from, float to) {
    return from + amount * (to - from);
}

double wrap(double value) {
    return value - std::floor(value / 33554432.0 + 0.5) * 33554432.0;
}

float gradient(std::uint8_t hash, float x, float y, float z) {
    const std::uint8_t h = hash & 15U;
    const float u = h < 8U ? x : y;
    const float v = h < 4U ? y : (h == 12U || h == 14U ? x : z);
    return ((h & 1U) == 0U ? u : -u) + ((h & 2U) == 0U ? v : -v);
}

std::string octaveKey(std::string_view key, std::string_view stack, std::size_t octave) {
    return std::string(key) + '/' + std::string(stack) + "/octave_" + std::to_string(octave);
}

} // namespace

std::uint64_t deriveSeed(std::int64_t worldSeed, std::string_view key) {
    std::uint64_t hash = kFnvOffset;
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return splitMix64(static_cast<std::uint64_t>(worldSeed) ^ splitMix64(hash));
}

PerlinNoise::PerlinNoise(std::uint64_t seed) {
    for (double& offset : offsets_) {
        seed = splitMix64(seed);
        offset = static_cast<double>(seed >> 11U) * 0x1p-53 * 256.0;
    }
    std::array<std::uint8_t, 256> values{};
    std::iota(values.begin(), values.end(), std::uint8_t{0});

    for (std::size_t i = values.size() - 1; i > 0; --i) {
        seed = splitMix64(seed);
        const std::size_t selected = static_cast<std::size_t>(seed % (i + 1));
        std::swap(values[i], values[selected]);
    }

    for (std::size_t i = 0; i < permutation_.size(); ++i) {
        permutation_[i] = values[i & 255U];
    }
}

float PerlinNoise::sample(double x, double y, double z, double smearScaleY) const {
    const double originalY = y;
    x = wrap(x) + offsets_[0];
    y = wrap(y) + offsets_[1];
    z = wrap(z) + offsets_[2];
    const double floorX = std::floor(x);
    const double floorY = std::floor(y);
    const double floorZ = std::floor(z);
    const int xi = static_cast<int>(floorX) & 255;
    const int yi = static_cast<int>(floorY) & 255;
    const int zi = static_cast<int>(floorZ) & 255;

    const float localX = static_cast<float>(x - floorX);
    const double relativeY = y - floorY;
    const double fudgeLimit = originalY >= 0.0 && originalY < relativeY ? originalY : relativeY;
    const double fudge = smearScaleY > 0.0
        ? std::floor(fudgeLimit / smearScaleY + 1.0e-7F) * smearScaleY : 0.0;
    const float localY = static_cast<float>(relativeY - fudge);
    const float localZ = static_cast<float>(z - floorZ);
    const float u = fade(localX);
    const float v = fade(static_cast<float>(relativeY));
    const float w = fade(localZ);

    const int a = permutation_[xi] + yi;
    const int aa = permutation_[a] + zi;
    const int ab = permutation_[a + 1] + zi;
    const int b = permutation_[xi + 1] + yi;
    const int ba = permutation_[b] + zi;
    const int bb = permutation_[b + 1] + zi;

    const float x00 = lerp(
        u,
        gradient(permutation_[aa], localX, localY, localZ),
        gradient(permutation_[ba], localX - 1.0F, localY, localZ)
    );
    const float x10 = lerp(
        u,
        gradient(permutation_[ab], localX, localY - 1.0F, localZ),
        gradient(permutation_[bb], localX - 1.0F, localY - 1.0F, localZ)
    );
    const float x01 = lerp(
        u,
        gradient(permutation_[aa + 1], localX, localY, localZ - 1.0F),
        gradient(permutation_[ba + 1], localX - 1.0F, localY, localZ - 1.0F)
    );
    const float x11 = lerp(
        u,
        gradient(permutation_[ab + 1], localX, localY - 1.0F, localZ - 1.0F),
        gradient(permutation_[bb + 1], localX - 1.0F, localY - 1.0F, localZ - 1.0F)
    );

    return lerp(w, lerp(v, x00, x10), lerp(v, x01, x11));
}

NormalNoise::NormalNoise(
    std::int64_t worldSeed,
    std::string_view key,
    int firstOctave,
    std::vector<double> amplitudes
) {
    double frequency = std::exp2(static_cast<double>(firstOctave));
    std::size_t first = amplitudes.size();
    std::size_t last = 0;
    for (std::size_t i = 0; i < amplitudes.size(); ++i) {
        if (amplitudes[i] != 0.0) {
            first = std::min(first, i);
            last = i;
        }
    }
    if (first == amplitudes.size()) {
        return;
    }

    // NoiseData uses NormalNoise.createParity: normalize by octave count,
    // then by the span of nonzero octaves, not by the sum of modifiers.
    const double normalization = (1.0 / 6.0) / (0.1 * (1.0 + 1.0 / (last - first + 1)));
    double persistence = 1.0 / (2.0 - std::exp2(1.0 - static_cast<double>(amplitudes.size())));
    for (std::size_t i = 0; i < amplitudes.size(); ++i) {
        if (amplitudes[i] != 0.0) {
            octaves_.push_back({
                PerlinNoise(deriveSeed(worldSeed, octaveKey(key, "a", i))),
                PerlinNoise(deriveSeed(worldSeed, octaveKey(key, "b", i))),
                frequency,
                static_cast<float>(amplitudes[i] * persistence * normalization),
            });
        }
        frequency *= 2.0;
        persistence *= 0.5;
    }
}

float NormalNoise::sample(double x, double y, double z) const {
    float value = 0.0F;
    for (const Octave& octave : octaves_) {
        const double frequency = octave.frequency;
        const float first = octave.first.sample(x * frequency, y * frequency, z * frequency);
        const float second = octave.second.sample(
            x * frequency * 1.0181268882175227,
            y * frequency * 1.0181268882175227,
            z * frequency * 1.0181268882175227
        );
        value += octave.amplitude * first;
        value += octave.amplitude * second;
    }
    return value;
}

BlendedNoise::BlendedNoise(std::int64_t worldSeed) {
    minLimit_.reserve(16);
    maxLimit_.reserve(16);
    main_.reserve(8);
    for (int i = 0; i < 16; ++i) {
        minLimit_.emplace_back(deriveSeed(worldSeed, "minecraft:terrain/min/" + std::to_string(i)));
        maxLimit_.emplace_back(deriveSeed(worldSeed, "minecraft:terrain/max/" + std::to_string(i)));
    }
    for (int i = 0; i < 8; ++i) {
        main_.emplace_back(deriveSeed(worldSeed, "minecraft:terrain/main/" + std::to_string(i)));
    }
}

float BlendedNoise::sampleFbm(
    const std::vector<PerlinNoise>& octaves,
    double x,
    double y,
    double z,
    int firstOctave,
    float valueFactor,
    double smearScaleY
) {
    double frequency = std::exp2(static_cast<double>(firstOctave));
    float amplitude = 1.0F;
    float totalAmplitude = 0.0F;
    float value = 0.0F;
    for (const PerlinNoise& octave : octaves) {
        value += amplitude * octave.sample(x * frequency, y * frequency, z * frequency, smearScaleY * frequency);
        totalAmplitude += amplitude;
        frequency *= 2.0;
        amplitude *= 0.5F;
    }
    return totalAmplitude == 0.0F ? 0.0F : valueFactor * value / totalAmplitude;
}

float BlendedNoise::sample(double x, double y, double z) const {
    constexpr double xzMultiplier = 684.412 * 0.25;
    constexpr double yMultiplier = 684.412 * 0.125;
    const float minValue = sampleFbm(
        minLimit_, x * xzMultiplier, y * yMultiplier, z * xzMultiplier, -15, 0.99998474F, yMultiplier * 8.0
    );
    const float maxValue = sampleFbm(
        maxLimit_, x * xzMultiplier, y * yMultiplier, z * xzMultiplier, -15, 0.99998474F, yMultiplier * 8.0
    );
    const float mainValue = sampleFbm(
        main_, x * xzMultiplier / 80.0, y * yMultiplier / 160.0, z * xzMultiplier / 80.0, -7, 12.75F, yMultiplier * 8.0 / 160.0
    );
    const float choice = std::clamp(mainValue + 0.5F, 0.0F, 1.0F);
    return lerp(choice, minValue, maxValue);
}

} // namespace mcworld::detail

#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mcworld::detail {

[[nodiscard]] std::uint64_t deriveSeed(std::int64_t worldSeed, std::string_view key);

class PerlinNoise {
public:
    explicit PerlinNoise(std::uint64_t seed);
    [[nodiscard]] float sample(double x, double y, double z, double smearScaleY = 0.0) const;

private:
    std::array<std::uint8_t, 512> permutation_{};
    std::array<double, 3> offsets_{};
};

class NormalNoise {
public:
    NormalNoise() = default;
    NormalNoise(std::int64_t worldSeed, std::string_view key, int firstOctave, std::vector<double> amplitudes);

    [[nodiscard]] float sample(double x, double y, double z) const;

private:
    struct Octave {
        PerlinNoise first;
        PerlinNoise second;
        double frequency{};
        float amplitude{};
    };

    std::vector<Octave> octaves_;
};

class BlendedNoise {
public:
    explicit BlendedNoise(std::int64_t worldSeed);
    [[nodiscard]] float sample(double x, double y, double z) const;

private:
    [[nodiscard]] static float sampleFbm(
        const std::vector<PerlinNoise>& octaves,
        double x,
        double y,
        double z,
        int firstOctave,
        float valueFactor,
        double smearScaleY
    );

    std::vector<PerlinNoise> minLimit_;
    std::vector<PerlinNoise> maxLimit_;
    std::vector<PerlinNoise> main_;
};

} // namespace mcworld::detail

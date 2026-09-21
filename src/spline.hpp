#pragma once

#include <memory>
#include <vector>

namespace mcworld::detail {

struct TerrainPoint {
    float continentalness{};
    float erosion{};
    float weirdness{};
    float ridges{};
};

enum class TerrainCoordinate {
    Continentalness,
    Erosion,
    Weirdness,
    Ridges,
};

class CubicSpline {
public:
    struct Point {
        float location{};
        float value{};
        std::shared_ptr<const CubicSpline> nested;
        float derivative{};
    };

    CubicSpline(TerrainCoordinate coordinate, std::vector<Point> points);
    [[nodiscard]] float sample(const TerrainPoint& point) const;

private:
    [[nodiscard]] float coordinate(const TerrainPoint& point) const;
    [[nodiscard]] static float valueAt(const Point& point, const TerrainPoint& input);

    TerrainCoordinate coordinate_;
    std::vector<Point> points_;
};

struct TerrainSplines {
    std::shared_ptr<const CubicSpline> offset;
    std::shared_ptr<const CubicSpline> factor;
    std::shared_ptr<const CubicSpline> jaggedness;
};

[[nodiscard]] TerrainSplines buildStandardOverworldSplines();
[[nodiscard]] float peaksAndValleys(float weirdness);

} // namespace mcworld::detail

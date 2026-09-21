#include "mcworld/isosurface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <vector>

namespace mcworld {
namespace {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Sample {
    Vec3 position;
    Vec3 gradient;
    float density{};
};

Vec3 operator+(const Vec3& left, const Vec3& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 operator-(const Vec3& left, const Vec3& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 operator*(const Vec3& value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

float dot(const Vec3& left, const Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 cross(const Vec3& left, const Vec3& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

Vec3 normalize(const Vec3& value) {
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= 1.0e-12F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return value * (1.0F / std::sqrt(lengthSquared));
}

Sample interpolate(const Sample& from, const Sample& to, float isoLevel) {
    const float difference = to.density - from.density;
    const float amount = std::abs(difference) < 1.0e-8F
        ? 0.5F
        : std::clamp((isoLevel - from.density) / difference, 0.0F, 1.0F);
    return {
        from.position + (to.position - from.position) * amount,
        from.gradient + (to.gradient - from.gradient) * amount,
        isoLevel,
    };
}

SurfaceVertex makeVertex(const Sample& sample) {
    const Vec3 normal = normalize(sample.gradient * -1.0F);
    return {
        sample.position.x,
        sample.position.y,
        sample.position.z,
        normal.x,
        normal.y,
        normal.z,
    };
}

void appendTriangle(SurfaceMesh& mesh, Sample a, Sample b, Sample c) {
    const Vec3 faceNormal = cross(b.position - a.position, c.position - a.position);
    if (dot(faceNormal, faceNormal) == 0.0F) {
        return;
    }
    const Vec3 desiredNormal = (a.gradient + b.gradient + c.gradient) * (-1.0F / 3.0F);
    if (dot(faceNormal, desiredNormal) < 0.0F) {
        std::swap(b, c);
    }
    mesh.vertices.push_back(makeVertex(a));
    mesh.vertices.push_back(makeVertex(b));
    mesh.vertices.push_back(makeVertex(c));
}

void polygonizeTetrahedron(
    SurfaceMesh& mesh,
    const std::array<Sample, 4>& points,
    float isoLevel
) {
    std::array<int, 4> inside{};
    std::array<int, 4> outside{};
    int insideCount = 0;
    int outsideCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (points[static_cast<std::size_t>(i)].density > isoLevel) {
            inside[static_cast<std::size_t>(insideCount++)] = i;
        } else {
            outside[static_cast<std::size_t>(outsideCount++)] = i;
        }
    }

    if (insideCount == 0 || insideCount == 4) {
        return;
    }
    if (insideCount == 1) {
        const Sample& solid = points[static_cast<std::size_t>(inside[0])];
        appendTriangle(
            mesh,
            interpolate(solid, points[static_cast<std::size_t>(outside[0])], isoLevel),
            interpolate(solid, points[static_cast<std::size_t>(outside[1])], isoLevel),
            interpolate(solid, points[static_cast<std::size_t>(outside[2])], isoLevel)
        );
        return;
    }
    if (insideCount == 3) {
        const Sample& air = points[static_cast<std::size_t>(outside[0])];
        appendTriangle(
            mesh,
            interpolate(air, points[static_cast<std::size_t>(inside[0])], isoLevel),
            interpolate(air, points[static_cast<std::size_t>(inside[1])], isoLevel),
            interpolate(air, points[static_cast<std::size_t>(inside[2])], isoLevel)
        );
        return;
    }

    const Sample a = interpolate(
        points[static_cast<std::size_t>(inside[0])], points[static_cast<std::size_t>(outside[0])], isoLevel
    );
    const Sample b = interpolate(
        points[static_cast<std::size_t>(inside[0])], points[static_cast<std::size_t>(outside[1])], isoLevel
    );
    const Sample c = interpolate(
        points[static_cast<std::size_t>(inside[1])], points[static_cast<std::size_t>(outside[0])], isoLevel
    );
    const Sample d = interpolate(
        points[static_cast<std::size_t>(inside[1])], points[static_cast<std::size_t>(outside[1])], isoLevel
    );
    appendTriangle(mesh, a, c, b);
    appendTriangle(mesh, b, c, d);
}

} // namespace

SurfaceMesh buildChunkIsosurface(const OverworldNoiseRouter& router, const ChunkSurfaceRequest& request) {
    if (request.maxY <= request.minY) {
        throw std::invalid_argument("Chunk surface maxY must be greater than minY");
    }
    if (request.minY < -64 || request.maxY > 320 || !std::isfinite(request.isoLevel)) {
        throw std::invalid_argument("Chunk surface requires finite isoLevel and Y bounds within [-64, 320]");
    }

    const std::int64_t worldMinX = static_cast<std::int64_t>(request.chunkX) * 16;
    const std::int64_t worldMinZ = static_cast<std::int64_t>(request.chunkZ) * 16;
    for (const auto origin : {worldMinX, worldMinZ}) {
        if (origin < static_cast<std::int64_t>(std::numeric_limits<int>::min()) + 16
            || origin > static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 32) {
            throw std::invalid_argument("Chunk coordinate is outside the supported integer grid");
        }
    }

    // Include a one-block halo so central-difference normals agree across chunks.
    constexpr int width = 19;
    constexpr int depth = 19;
    const int height = request.maxY - request.minY + 1;
    const auto index = [height](int x, int y, int z) {
        return static_cast<std::size_t>(((z + 1) * width + x + 1) * (height + 2) + y + 1);
    };

    std::vector<float> densities(static_cast<std::size_t>(width * depth * (height + 2)));
    for (int z = -1; z <= 17; ++z) {
        for (int x = -1; x <= 17; ++x) {
            for (int y = -1; y <= height; ++y) {
                densities[index(x, y, z)] = router.sampleFinalDensity(
                    worldMinX + x,
                    request.minY + y,
                    worldMinZ + z
                );
            }
        }
    }

    const auto density = [&](int x, int y, int z) {
        return densities[index(x, y, z)];
    };
    const auto point = [&](int x, int y, int z) {
        return Sample{
            Vec3{static_cast<float>(x), static_cast<float>(request.minY + y), static_cast<float>(z)},
            Vec3{
                density(x + 1, y, z) - density(x - 1, y, z),
                density(x, y + 1, z) - density(x, y - 1, z),
                density(x, y, z + 1) - density(x, y, z - 1),
            },
            density(x, y, z),
        };
    };

    constexpr std::array<std::array<int, 3>, 8> cornerOffsets{{
        {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
        {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}},
    }};
    constexpr std::array<std::array<int, 4>, 6> tetrahedra{{
        {{0, 5, 1, 6}}, {{0, 1, 2, 6}}, {{0, 2, 3, 6}},
        {{0, 3, 7, 6}}, {{0, 7, 4, 6}}, {{0, 4, 5, 6}},
    }};

    SurfaceMesh mesh;
    mesh.vertices.reserve(100'000);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < height - 1; ++y) {
                std::array<Sample, 8> cube{};
                for (std::size_t corner = 0; corner < cube.size(); ++corner) {
                    const auto& offset = cornerOffsets[corner];
                    cube[corner] = point(x + offset[0], y + offset[1], z + offset[2]);
                }
                for (const auto& tetrahedron : tetrahedra) {
                    polygonizeTetrahedron(
                        mesh,
                        {{
                            cube[static_cast<std::size_t>(tetrahedron[0])],
                            cube[static_cast<std::size_t>(tetrahedron[1])],
                            cube[static_cast<std::size_t>(tetrahedron[2])],
                            cube[static_cast<std::size_t>(tetrahedron[3])],
                        }},
                        request.isoLevel
                    );
                }
            }
        }
    }
    return mesh;
}

} // namespace mcworld

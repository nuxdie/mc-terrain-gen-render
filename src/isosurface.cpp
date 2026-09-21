// Extracts the density = isoLevel surface of one chunk as a triangle soup.
//
// Marching *tetrahedra* rather than marching cubes: each cell is split into six
// tetrahedra sharing the 0-6 diagonal, which needs no 256-entry case table and
// cannot produce the ambiguous-face cracks marching cubes is prone to, at the
// cost of more triangles. Normals come from central differences of the density
// field, which is why the sampled grid carries a one-block halo.

#include "mcworld/isosurface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mcworld {
namespace {

constexpr int kChunkSize = 16;
// One block of halo on every side, so the central differences used for normals
// have a valid neighbour even for cells on the chunk boundary.
constexpr int kHalo = 1;

// The world-space corner a chunk's local coordinates are measured from.
// Held as int64 so the overflow check below can run before any narrowing.
struct ChunkOrigin {
    std::int64_t x{};
    std::int64_t z{};
    int minY{};
};

[[nodiscard]] ChunkOrigin originOf(const ChunkSurfaceRequest& request) {
    return {
        static_cast<std::int64_t>(request.chunkX) * kChunkSize,
        static_cast<std::int64_t>(request.chunkZ) * kChunkSize,
        request.minY,
    };
}

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

// A grid corner: where it is, which way the density field is falling, and how
// solid it is. The gradient is carried alongside so it can be interpolated to
// the crossing point rather than recomputed there.
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
        return {0.0F, 1.0F, 0.0F}; // Degenerate gradient: point straight up.
    }
    return value * (1.0F / std::sqrt(lengthSquared));
}

// Where the edge from `from` to `to` crosses the iso level. A vanishing density
// difference means the edge lies along the surface, so split it down the middle.
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
    // Density rises into solid, so the outward normal is the negated gradient.
    const Vec3 normal = normalize(sample.gradient * -1.0F);
    return {sample.position.x, sample.position.y, sample.position.z, normal.x, normal.y, normal.z};
}

void appendTriangle(SurfaceMesh& mesh, Sample a, Sample b, Sample c) {
    const Vec3 faceNormal = cross(b.position - a.position, c.position - a.position);
    if (dot(faceNormal, faceNormal) == 0.0F) {
        return; // Degenerate: the three crossings are collinear.
    }
    // Orient the winding so the geometric normal agrees with the density
    // gradient, instead of relying on the corner ordering of each tetrahedron.
    const Vec3 desiredNormal = (a.gradient + b.gradient + c.gradient) * (-1.0F / 3.0F);
    if (dot(faceNormal, desiredNormal) < 0.0F) {
        std::swap(b, c);
    }
    mesh.vertices.push_back(makeVertex(a));
    mesh.vertices.push_back(makeVertex(b));
    mesh.vertices.push_back(makeVertex(c));
}

// One tetrahedron contributes nothing (all corners on one side), one triangle
// (a 1-3 split) or two (a 2-2 split).
void polygonizeTetrahedron(SurfaceMesh& mesh, const std::array<Sample, 4>& corners, float isoLevel) {
    std::array<const Sample*, 4> solid{};
    std::array<const Sample*, 4> air{};
    std::size_t solidCount = 0;
    std::size_t airCount = 0;
    for (const Sample& corner : corners) {
        if (corner.density > isoLevel) {
            solid[solidCount++] = &corner;
        } else {
            air[airCount++] = &corner;
        }
    }

    switch (solidCount) {
    case 0:
    case 4:
        return;

    case 1: // One solid corner is cut off as a triangle.
        appendTriangle(
            mesh,
            interpolate(*solid[0], *air[0], isoLevel),
            interpolate(*solid[0], *air[1], isoLevel),
            interpolate(*solid[0], *air[2], isoLevel)
        );
        return;

    case 3: // Mirror image: one air corner is cut off.
        appendTriangle(
            mesh,
            interpolate(*air[0], *solid[0], isoLevel),
            interpolate(*air[0], *solid[1], isoLevel),
            interpolate(*air[0], *solid[2], isoLevel)
        );
        return;

    default: { // 2-2 split: the surface crosses four edges, forming a quad.
        const Sample a = interpolate(*solid[0], *air[0], isoLevel);
        const Sample b = interpolate(*solid[0], *air[1], isoLevel);
        const Sample c = interpolate(*solid[1], *air[0], isoLevel);
        const Sample d = interpolate(*solid[1], *air[1], isoLevel);
        appendTriangle(mesh, a, c, b);
        appendTriangle(mesh, b, c, d);
        return;
    }
    }
}

// Densities for one chunk plus its halo, indexed in chunk-local coordinates
// where x and z run [-kHalo, kChunkSize + kHalo] and y runs [-kHalo, height].
class DensityGrid {
public:
    DensityGrid(const OverworldNoiseRouter& router, const ChunkOrigin& origin, int height)
        : strideY_(height + 2 * kHalo),
          values_(static_cast<std::size_t>(kSpan) * kSpan * (height + 2 * kHalo)) {
        for (int z = -kHalo; z <= kChunkSize + kHalo; ++z) {
            for (int x = -kHalo; x <= kChunkSize + kHalo; ++x) {
                for (int y = -kHalo; y < height + kHalo; ++y) {
                    values_[index(x, y, z)] =
                        router.sampleFinalDensity(origin.x + x, origin.minY + y, origin.z + z);
                }
            }
        }
    }

    [[nodiscard]] float at(int x, int y, int z) const {
        return values_[index(x, y, z)];
    }

private:
    // Sample positions per horizontal axis: the chunk, its halo on both sides,
    // and the far corner shared with the next chunk.
    static constexpr int kSpan = kChunkSize + 2 * kHalo + 1;

    [[nodiscard]] std::size_t index(int x, int y, int z) const {
        return static_cast<std::size_t>(((z + kHalo) * kSpan + x + kHalo) * strideY_ + y + kHalo);
    }

    int strideY_;
    std::vector<float> values_;
};

// Cube corners in the order the tetrahedron table below indexes them.
constexpr std::array<std::array<int, 3>, 8> kCornerOffsets{{
    {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
    {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}},
}};

// Six tetrahedra tiling the cube, all sharing the corner 0 to corner 6 diagonal.
// Every neighbouring cell splits the same way, so shared faces always agree and
// the mesh stays watertight in the interior.
constexpr std::array<std::array<int, 4>, 6> kTetrahedra{{
    {{0, 5, 1, 6}}, {{0, 1, 2, 6}}, {{0, 2, 3, 6}},
    {{0, 3, 7, 6}}, {{0, 7, 4, 6}}, {{0, 4, 5, 6}},
}};

void validate(const ChunkSurfaceRequest& request) {
    if (request.maxY <= request.minY) {
        throw std::invalid_argument("Chunk surface maxY must be greater than minY");
    }
    if (request.minY < -64 || request.maxY > 320 || !std::isfinite(request.isoLevel)) {
        throw std::invalid_argument("Chunk surface requires finite isoLevel and Y bounds within [-64, 320]");
    }

    const ChunkOrigin origin = originOf(request);
    for (const std::int64_t axis : {origin.x, origin.z}) {
        // Leave room for the halo below and for the halo plus a full chunk
        // above, so every sampled coordinate stays representable as an int.
        if (axis < static_cast<std::int64_t>(std::numeric_limits<int>::min()) + kChunkSize
            || axis > static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 2 * kChunkSize) {
            throw std::invalid_argument("Chunk coordinate is outside the supported integer grid");
        }
    }
}

} // namespace

SurfaceMesh buildChunkIsosurface(const OverworldNoiseRouter& router, const ChunkSurfaceRequest& request) {
    validate(request);

    const int height = request.maxY - request.minY + 1;
    const DensityGrid grid(router, originOf(request), height);

    const auto cornerAt = [&](int x, int y, int z) {
        return Sample{
            Vec3{static_cast<float>(x), static_cast<float>(request.minY + y), static_cast<float>(z)},
            Vec3{
                grid.at(x + 1, y, z) - grid.at(x - 1, y, z),
                grid.at(x, y + 1, z) - grid.at(x, y - 1, z),
                grid.at(x, y, z + 1) - grid.at(x, y, z - 1),
            },
            grid.at(x, y, z),
        };
    };

    SurfaceMesh mesh;
    mesh.vertices.reserve(100'000);
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            for (int y = 0; y < height - 1; ++y) {
                std::array<Sample, 8> cube{};
                for (std::size_t corner = 0; corner < cube.size(); ++corner) {
                    const std::array<int, 3>& offset = kCornerOffsets[corner];
                    cube[corner] = cornerAt(x + offset[0], y + offset[1], z + offset[2]);
                }
                for (const std::array<int, 4>& tetrahedron : kTetrahedra) {
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

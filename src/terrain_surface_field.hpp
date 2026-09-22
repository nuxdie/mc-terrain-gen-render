#pragma once

#include <algorithm>
#include <cmath>

namespace viewer::detail {

// Reconstruct only the edits made after density fill. Filtering the correction
// (rather than the original density) leaves unedited 7A terrain exactly intact.
inline float terrainCorrection(float density, bool solid) {
    if (solid == (density > 0.0F)) return 0.0F;
    const float target = (solid ? 1.0F : -1.0F) * std::max(std::abs(density), 0.05F);
    return target - density;
}

template <typename Correction>
float filteredCorrection(int x, int y, int z, const Correction& correction) {
    // Separable [1, 4, 1] reconstruction kernel. The stronger central weight
    // preserves one-block-thick sheets (cave roofs / aquifer barriers) while
    // rounding steps. The reconstructed boundary
    // depends on its 3-D neighborhood instead of snapping each edited block to
    // a sign. Do not clamp the resulting field back to block occupancy.
    float sum = 0.0F;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                const int weight = (dx == 0 ? 4 : 1) * (dy == 0 ? 4 : 1) * (dz == 0 ? 4 : 1);
                sum += weight * correction(x + dx, y + dy, z + dz);
            }
        }
    }
    return sum / 216.0F;
}

} // namespace viewer::detail

#pragma once

// Scalar primitives shared by the noise, spline and density-router layers.
//
// Each function mirrors one operation in the Minecraft 26.3 density-function
// vocabulary; the names follow the Java side so the graph in
// `minecraft-26.3-worldgen.dot` stays readable against this code.
//
// The float/double split is deliberate and load-bearing: sample coordinates are
// doubles (they carry world positions far from the origin) while density values
// are floats (Minecraft evaluates and stores them as floats). Widening or
// narrowing either side changes generated terrain, so keep the signatures.

#include <algorithm>

namespace mcworld::detail {

[[nodiscard]] inline float lerp(float amount, float from, float to) {
    return from + amount * (to - from);
}

// Linear ramp between two Y planes, clamped outside them. Java: `yClampedGradient`.
[[nodiscard]] inline float clampedGradient(double value, double fromY, double toY, float fromValue, float toValue) {
    const float amount = static_cast<float>(std::clamp((value - fromY) / (toY - fromY), 0.0, 1.0));
    return lerp(amount, fromValue, toValue);
}

// Affine rescale from one interval to another, without clamping.
[[nodiscard]] inline float remap(float value, float fromLow, float fromHigh, float toLow, float toHigh) {
    return toLow + (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow);
}

// Map a roughly [-1, 1] noise sample onto [low, high]. Java: `mapRange`.
// `low` may exceed `high`, which flips the sense of the noise.
[[nodiscard]] inline float mapNoise(float value, float low, float high) {
    return value * (high - low) * 0.5F + (high + low) * 0.5F;
}

// Attenuate negative densities so caves bite less deeply than terrain rises.
[[nodiscard]] inline float halfNegative(float value) {
    return value > 0.0F ? value : value * 0.5F;
}

[[nodiscard]] inline float quarterNegative(float value) {
    return value > 0.0F ? value : value * 0.25F;
}

// Soft clip applied once to the interpolated density. Java: `squeeze`.
[[nodiscard]] inline float squeeze(float value) {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    return clamped * 0.5F - clamped * clamped * clamped / 24.0F;
}

} // namespace mcworld::detail

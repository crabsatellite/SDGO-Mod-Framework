#pragma once

#include <algorithm>
#include <cmath>

namespace sdgo::login_showcase_fit
{
    struct Bounds
    {
        float min_x, min_y, min_z;
        float max_x, max_y, max_z;
    };

    constexpr float kTemplateScale = 20.0f;
    constexpr float kReferenceWidth = 26.0f;
    constexpr float kReferenceHeight = 26.0f;
    constexpr float kReferenceDepth = 30.0f;
    constexpr float kMaximumTrustedCoordinate = 100000.0f;
    constexpr float kMaximumTrustedExtent = 10000.0f;

    inline bool ReplacementScale(
        const Bounds& bounds, float native_scale, float& fitted_scale) noexcept
    {
        fitted_scale = native_scale;
        const float values[] = {
            bounds.min_x, bounds.min_y, bounds.min_z,
            bounds.max_x, bounds.max_y, bounds.max_z,
            native_scale
        };
        for (float value : values) {
            if (!std::isfinite(value) ||
                std::fabs(value) > kMaximumTrustedCoordinate)
                return false;
        }
        if (native_scale <= 0.0f)
            return false;

        const float width = bounds.max_x - bounds.min_x;
        const float height = bounds.max_y - bounds.min_y;
        const float depth = bounds.max_z - bounds.min_z;
        if (!std::isfinite(width) || !std::isfinite(height) ||
            !std::isfinite(depth) ||
            width <= 0.0f || height <= 0.0f || depth <= 0.0f ||
            width > kMaximumTrustedExtent ||
            height > kMaximumTrustedExtent ||
            depth > kMaximumTrustedExtent)
            return false;

        const float ratio = std::min(
            {1.0f, kReferenceWidth / width,
             kReferenceHeight / height, kReferenceDepth / depth});
        const float bounded = kTemplateScale * ratio;
        if (!std::isfinite(bounded) || bounded <= 0.0f)
            return false;
        fitted_scale = std::min(native_scale, bounded);
        return std::isfinite(fitted_scale) && fitted_scale > 0.0f;
    }
}

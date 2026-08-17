#pragma once

#include <algorithm>
#include <cmath>

namespace interaction_motion {

inline double TrackRatio(int pointerX, int inset, int usableWidth) {
    const int width = std::max(1, usableWidth);
    return std::clamp(static_cast<double>(pointerX - inset) / width, 0.0, 1.0);
}

inline int LogicalSliderValue(double ratio, int minimum, int maximum) {
    const double clamped = std::clamp(ratio, 0.0, 1.0);
    return minimum + static_cast<int>(std::lround(clamped * (maximum - minimum)));
}

inline int VisualSliderX(double ratio, int inset, int usableWidth) {
    return inset + static_cast<int>(std::lround(
        std::clamp(ratio, 0.0, 1.0) * std::max(1, usableWidth)));
}

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool operator==(const Rect&) const = default;
};

inline bool ShouldRetarget(bool animationActive, const Rect& activeTarget,
                           const Rect& nextTarget) {
    return !animationActive || activeTarget != nextTarget;
}

inline Rect AnimateRect(const Rect& from, const Rect& target,
                        double elapsedMs, double durationMs) {
    const double linear = durationMs <= 0.0
        ? 1.0 : std::clamp(elapsedMs / durationMs, 0.0, 1.0);
    // Smoothstep starts and lands with zero velocity. It avoids the large
    // first-frame leap of a cubic ease-out when a whole grid unit is added.
    const double eased = linear * linear * (3.0 - 2.0 * linear);
    const auto interpolate = [&](int start, int finish) {
        return start + static_cast<int>(std::lround((finish - start) * eased));
    };
    return {
        interpolate(from.left, target.left),
        interpolate(from.top, target.top),
        interpolate(from.right, target.right),
        interpolate(from.bottom, target.bottom),
    };
}

} // namespace interaction_motion

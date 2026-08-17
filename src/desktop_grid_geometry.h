#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace desktop_grid_geometry {

struct CollapsedLayout {
    RECT tile{};
    RECT label{};
};

inline int VisualUnit(int pitchX, int pitchY) {
    return std::max(1, std::min(pitchX, pitchY));
}

inline CollapsedLayout MakeCollapsedLayout(int slotWidth, int slotHeight, int dpi) {
    const auto scale = [dpi](int value) {
        return std::max(1, static_cast<int>(MulDiv(value, dpi, 96)));
    };
    const int sideInset = scale(4);
    const int topInset = scale(2);
    const int gap = scale(4);
    const int labelHeight = scale(24);
    const int bottomInset = scale(2);
    const int side = std::max(1, std::min(
        slotWidth - sideInset * 2,
        slotHeight - topInset - gap - labelHeight - bottomInset));
    const int left = (slotWidth - side) / 2;
    const int labelTop = topInset + side + gap;
    return {
        {left, topInset, left + side, topInset + side},
        {scale(2), labelTop, slotWidth - scale(2), slotHeight - bottomInset},
    };
}

inline int SnapIndex(int position, int origin, int pitch) {
    if (pitch <= 0) return 0;
    return static_cast<int>(std::lround(
        static_cast<double>(position - origin) / static_cast<double>(pitch)));
}

inline int SnapCoordinate(int position, int origin, int pitch) {
    return origin + SnapIndex(position, origin, pitch) * std::max(1, pitch);
}

inline int SnapIndexWithHysteresis(int position, int origin, int pitch,
                                   int previousIndex, double threshold = 0.58) {
    if (pitch <= 0) return previousIndex;
    const double logical = static_cast<double>(position - origin) / pitch;
    if (logical > previousIndex + threshold || logical < previousIndex - threshold)
        return static_cast<int>(std::lround(logical));
    return previousIndex;
}

} // namespace desktop_grid_geometry

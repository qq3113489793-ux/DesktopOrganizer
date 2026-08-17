#pragma once

#include "model.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <vector>

namespace layout {

inline int CellSize(int unit, IconSize size) {
    if (size == IconSize::Small) return std::max(1, unit / 2);
    if (size == IconSize::Large) return unit * 2;
    return unit;
}

inline int Capacity(int columns, int rows, int unit, IconSize size) {
    const int cell = CellSize(unit, size);
    return std::max(1, columns * unit / cell) * std::max(1, rows * unit / cell);
}

inline int SnapCount(int contentPixels, int unit) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(contentPixels) / unit)));
}

inline double EaseOutCubic(double progress) {
    const double t = std::clamp(progress, 0.0, 1.0);
    const double remaining = 1.0 - t;
    return 1.0 - remaining * remaining * remaining;
}

inline bool ExpandToFit(int& columns, int& rows, int unit, IconSize size,
                        std::size_t itemCount, int maxColumns, int maxRows) {
    while (Capacity(columns, rows, unit, size) < static_cast<int>(itemCount)) {
        if (columns < maxColumns) ++columns;
        else if (rows < maxRows) ++rows;
        else return false;
    }
    return true;
}

struct Placement {
    int x = 0;
    int y = 0;
    int span = 1;
};

inline bool CanPlace(const std::vector<unsigned char>& occupied, int width, int height,
                     int x, int y, int span) {
    if (x < 0 || y < 0 || x + span > width || y + span > height) return false;
    for (int row = y; row < y + span; ++row)
        for (int column = x; column < x + span; ++column)
            if (occupied[static_cast<std::size_t>(row * width + column)]) return false;
    return true;
}

inline void Occupy(std::vector<unsigned char>& occupied, int width, const Placement& placement) {
    for (int row = placement.y; row < placement.y + placement.span; ++row)
        for (int column = placement.x; column < placement.x + placement.span; ++column)
            occupied[static_cast<std::size_t>(row * width + column)] = 1;
}

inline int Span(IconSize size) {
    if (size == IconSize::Small) return 1;
    if (size == IconSize::Large) return 4;
    return 2;
}

inline bool PackItems(int columns, int rows, const std::vector<IconSize>& sizes,
                      std::vector<Placement>* placements = nullptr) {
    const int width = columns * 2;
    const int height = rows * 2;
    std::vector<unsigned char> occupied(static_cast<std::size_t>(width * height), 0);
    std::vector<Placement> result;
    result.reserve(sizes.size());
    for (const IconSize size : sizes) {
        const int span = Span(size);
        bool placed = false;
        for (int y = 0; y + span <= height && !placed; ++y) {
            for (int x = 0; x + span <= width && !placed; ++x) {
                bool free = true;
                for (int row = y; row < y + span && free; ++row)
                    for (int column = x; column < x + span; ++column)
                        if (occupied[static_cast<std::size_t>(row * width + column)]) { free = false; break; }
                if (!free) continue;
                for (int row = y; row < y + span; ++row)
                    for (int column = x; column < x + span; ++column)
                        occupied[static_cast<std::size_t>(row * width + column)] = 1;
                result.push_back({x, y, span});
                placed = true;
            }
        }
        if (!placed) return false;
    }
    if (placements) *placements = std::move(result);
    return true;
}

inline bool ArrangeItemsInHalfUnits(int halfColumns, int halfRows,
                                    const std::vector<OrganizerItem>& items,
                                    std::vector<Placement>* placements = nullptr);

inline bool ArrangeItems(int columns, int rows, const std::vector<OrganizerItem>& items,
                         std::vector<Placement>* placements = nullptr) {
    return ArrangeItemsInHalfUnits(columns * 2, rows * 2, items, placements);
}

// The same packing rules directly in half-cell units. Containers can have a
// half-grid extra (1.5 x 1.5 = 3 x 3 half cells), which cannot be expressed
// through the whole-cell wrapper above.
inline bool ArrangeItemsInHalfUnits(int halfColumns, int halfRows,
                                    const std::vector<OrganizerItem>& items,
                                    std::vector<Placement>* placements) {
    const int width = halfColumns;
    const int height = halfRows;
    std::vector<unsigned char> occupied(static_cast<std::size_t>(width * height), 0);
    std::vector<Placement> result(items.size());

    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const int span = Span(item.iconSize);
        result[index] = {item.gridX, item.gridY, span};
        if (item.gridX < 0 || item.gridY < 0) continue;
        if (!CanPlace(occupied, width, height, item.gridX, item.gridY, span)) return false;
        Occupy(occupied, width, result[index]);
    }

    for (size_t index = 0; index < items.size(); ++index) {
        if (result[index].x >= 0 && result[index].y >= 0) continue;
        bool placed = false;
        for (int y = 0; y + result[index].span <= height && !placed; ++y) {
            for (int x = 0; x + result[index].span <= width && !placed; ++x) {
                Placement candidate{x, y, result[index].span};
                if (!CanPlace(occupied, width, height, x, y, candidate.span)) continue;
                result[index] = candidate;
                Occupy(occupied, width, candidate);
                placed = true;
            }
        }
        if (!placed) return false;
    }
    if (placements) *placements = std::move(result);
    return true;
}

inline bool FindNearestFreeInHalfUnits(int halfColumns, int halfRows,
                                       const std::vector<Placement>& placements,
                                       size_t ignoredIndex, int span, int desiredX, int desiredY,
                                       Placement& result);
inline bool FindNearestFree(int columns, int rows, const std::vector<Placement>& placements,
                            size_t ignoredIndex, int span, int desiredX, int desiredY,
                            Placement& result) {
    return FindNearestFreeInHalfUnits(columns * 2, rows * 2, placements, ignoredIndex,
                                      span, desiredX, desiredY, result);
}

inline bool FindNearestFreeInHalfUnits(int halfColumns, int halfRows,
                                       const std::vector<Placement>& placements,
                                       size_t ignoredIndex, int span, int desiredX, int desiredY,
                                       Placement& result) {
    const int width = halfColumns;
    const int height = halfRows;
    std::vector<unsigned char> occupied(static_cast<std::size_t>(width * height), 0);
    for (size_t index = 0; index < placements.size(); ++index) {
        if (index != ignoredIndex) Occupy(occupied, width, placements[index]);
    }

    int bestDistance = INT_MAX;
    bool found = false;
    for (int y = 0; y + span <= height; ++y) {
        for (int x = 0; x + span <= width; ++x) {
            if (!CanPlace(occupied, width, height, x, y, span)) continue;
            const int dx = x - desiredX;
            const int dy = y - desiredY;
            const int distance = dx * dx + dy * dy;
            if (distance >= bestDistance) continue;
            bestDistance = distance;
            result = {x, y, span};
            found = true;
        }
    }
    return found;
}

} // namespace layout

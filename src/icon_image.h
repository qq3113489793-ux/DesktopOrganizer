#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace icon_image {

struct Bounds {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool Empty() const { return right <= left || bottom <= top; }
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

inline bool Visible(std::uint32_t pixel) {
    return ((pixel >> 24) & 0xffu) > 8u || (pixel & 0x00ffffffu) != 0u;
}

inline Bounds VisibleBounds(const std::vector<std::uint32_t>& pixels, int width, int height) {
    Bounds result{width, height, 0, 0};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!Visible(pixels[static_cast<std::size_t>(y) * width + x])) continue;
            result.left = std::min(result.left, x);
            result.top = std::min(result.top, y);
            result.right = std::max(result.right, x + 1);
            result.bottom = std::max(result.bottom, y + 1);
        }
    }
    if (result.right == 0 || result.bottom == 0) return {};
    return result;
}

inline std::vector<std::uint32_t> FitVisiblePixels(const std::vector<std::uint32_t>& source,
                                                   int width, int height, int outputSize,
                                                   double fillRatio = 0.88) {
    if (width <= 0 || height <= 0 || outputSize <= 0) return {};
    std::vector<std::uint32_t> output(static_cast<std::size_t>(outputSize) * outputSize, 0);
    const Bounds bounds = VisibleBounds(source, width, height);
    if (bounds.Empty()) return output;

    const int maximum = std::max(1, static_cast<int>(std::lround(outputSize * fillRatio)));
    const double scale = std::min(static_cast<double>(maximum) / bounds.Width(),
                                  static_cast<double>(maximum) / bounds.Height());
    const int targetWidth = std::max(1, static_cast<int>(std::lround(bounds.Width() * scale)));
    const int targetHeight = std::max(1, static_cast<int>(std::lround(bounds.Height() * scale)));
    const int targetLeft = (outputSize - targetWidth) / 2;
    const int targetTop = (outputSize - targetHeight) / 2;

    const auto normalizedPixel = [&](int x, int y) {
        std::uint32_t pixel = source[static_cast<std::size_t>(y) * width + x];
        if (((pixel >> 24) & 0xffu) == 0u && (pixel & 0x00ffffffu) != 0u) pixel |= 0xff000000u;
        return pixel;
    };

    for (int y = 0; y < targetHeight; ++y) {
        const double sourceY = bounds.top + (y + 0.5) * bounds.Height() / targetHeight - 0.5;
        const int top = std::clamp(static_cast<int>(std::floor(sourceY)), bounds.top, bounds.bottom - 1);
        const int bottom = std::min(top + 1, bounds.bottom - 1);
        const double vertical = std::clamp(sourceY - top, 0.0, 1.0);
        for (int x = 0; x < targetWidth; ++x) {
            const double sourceX = bounds.left + (x + 0.5) * bounds.Width() / targetWidth - 0.5;
            const int left = std::clamp(static_cast<int>(std::floor(sourceX)), bounds.left, bounds.right - 1);
            const int right = std::min(left + 1, bounds.right - 1);
            const double horizontal = std::clamp(sourceX - left, 0.0, 1.0);
            const std::uint32_t samples[]{normalizedPixel(left, top), normalizedPixel(right, top),
                                          normalizedPixel(left, bottom), normalizedPixel(right, bottom)};
            std::uint32_t pixel = 0;
            for (int channel = 0; channel < 4; ++channel) {
                const int shift = channel * 8;
                const double upper = ((samples[0] >> shift) & 0xffu) * (1.0 - horizontal) +
                                     ((samples[1] >> shift) & 0xffu) * horizontal;
                const double lower = ((samples[2] >> shift) & 0xffu) * (1.0 - horizontal) +
                                     ((samples[3] >> shift) & 0xffu) * horizontal;
                pixel |= static_cast<std::uint32_t>(std::lround(upper * (1.0 - vertical) + lower * vertical)) << shift;
            }
            output[static_cast<std::size_t>(targetTop + y) * outputSize + targetLeft + x] = pixel;
        }
    }
    return output;
}

} // namespace icon_image

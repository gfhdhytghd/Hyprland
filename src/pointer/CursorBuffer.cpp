#include "CursorBuffer.hpp"
#include <array>
#include <algorithm>
#include <limits>

bool Pointer::packCursorARGB8888(std::span<const uint8_t> rgba, std::span<uint8_t> destination, size_t width, size_t height, size_t stride) {
    if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / 4)
        return false;

    const auto rowBytes = width * 4;
    if (stride < rowBytes || height > rgba.size() / rowBytes || height - 1 > destination.size() / stride)
        return false;
    if (rowBytes > destination.size() - (height - 1) * stride)
        return false;

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const auto offset = (y * width + x) * 4;
            // Keep the shader's premultiplied encoded values, including alpha.
            const std::array<uint8_t, 4> pixel = {rgba[offset + 2], rgba[offset + 1], rgba[offset], rgba[offset + 3]};
            std::ranges::copy(pixel, destination.data() + y * stride + x * 4);
        }
    }
    return true;
}

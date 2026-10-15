#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Pointer {
    // Pack RGBA bytes into DRM ARGB8888 (little-endian BGRA).
    // RPT_FB readback rows already have scanout order; do not vertically flip them.
    bool packCursorARGB8888(std::span<const uint8_t> rgba, std::span<uint8_t> destination, size_t width, size_t height, size_t stride);
}

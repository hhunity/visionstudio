#pragma once
#include <array>
#include <cstdint>
#include <vector>

struct image_data {
    std::vector<uint8_t> pixels; // RGBA, 4 bytes per pixel, row-major, top-to-bottom
    int width  = 0;
    int height = 0;

    bool empty() const { return pixels.empty(); }

    // Returns the RGBA value at (x, y), or {0,0,0,0} if out of bounds.
    std::array<uint8_t, 4> pixel_at(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return {0, 0, 0, 0};
        const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        return {pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]};
    }
};

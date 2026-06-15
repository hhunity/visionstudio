#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifdef IO_BUILD_DLL
#    define IO_API __declspec(dllexport)
#  else
#    define IO_API __declspec(dllimport)
#  endif
#else
#  define IO_API
#endif

enum class PixelFormat {
    rgba, // 4 bytes per pixel (R,G,B,A)
    rgb,  // 3 bytes per pixel (R,G,B)
    gray, // 1 byte per pixel (luminance)
};

// Row-major, top-left origin, 8-bit per channel.
struct image_data
{
    int         width  = 0;
    int         height = 0;
    PixelFormat format = PixelFormat::rgba;
    std::vector<uint8_t> pixels; // w * h * channels() bytes

    bool empty() const { return pixels.empty(); }
    int  channels() const {
        switch (format) {
            case PixelFormat::rgba: return 4;
            case PixelFormat::rgb:  return 3;
            case PixelFormat::gray: return 1;
        }
        return 4;
    }

    // Returns the RGBA value at (x, y), or {0,0,0,0} if out of bounds.
    // Note: only meaningful when format == PixelFormat::rgba.
    std::array<uint8_t, 4> pixel_at(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return {0, 0, 0, 0};
        const int    ch  = channels();
        const size_t idx = (static_cast<size_t>(y) * width + x) * ch;
        const uint8_t r = pixels[idx];
        const uint8_t g = ch > 1 ? pixels[idx + 1] : r;
        const uint8_t b = ch > 2 ? pixels[idx + 2] : r;
        const uint8_t a = ch > 3 ? pixels[idx + 3] : 255;
        return {r, g, b, a};
    }
};

// ---------------------------------------------------------------------------

namespace tiff_io {

// Options for write().  All fields have sensible defaults.
struct WriteOptions
{
    uint32_t    tile_size         = 512;               // tile width/height in pixels (must be a multiple of 16); 0 = strip layout
    // Compression scheme
    enum class Compression { none, deflate, lzw, packbits, zstd } compression = Compression::deflate;
    int         compression_level = 6;                 // zlib/zstd level: 1 = fastest, 9 = best ratio, 0 = none (when compression==deflate)
    PixelFormat output_format     = PixelFormat::rgb;  // output pixel format
    int         max_threads       = 0;                 // 0 = hardware concurrency (no cap); positive = explicit limit
    uint32_t    rows_per_strip    = 64;                // rows per strip when tile_size == 0
};

// Options for read().  All fields have sensible defaults.
struct ReadOptions
{
    int         max_threads   = 0;                     // 0 = hardware concurrency (no cap); positive = explicit limit
    PixelFormat output_format = PixelFormat::rgb;      // desired pixel format
};

// Probe a TIFF file and return its native pixel format without reading pixels.
// Returns PixelFormat::rgba on failure or unknown format.
IO_API PixelFormat detect_format(const std::string& path);

// Read a TIFF file into image_data.
// progress: optional atomic updated 0.0->1.0 as rows are decoded.
// Returns true on success.
IO_API bool read(const std::string& path, image_data& out,
                 std::atomic<float>* progress = nullptr,
                 const ReadOptions& opts = {});

// Write image_data to a TIFF file.
// tile_size > 0: tiled DEFLATE layout with parallel per-tile compression.
// tile_size == 0: strip DEFLATE layout with parallel per-strip compression.
// progress: optional atomic updated 0.0->1.0 as segments are written.
// Returns true on success.
IO_API bool write(const std::string& path, const image_data& img,
                  std::atomic<float>* progress = nullptr,
                  const WriteOptions& opts = {});

} // namespace tiff_io

#include "io/tiff_io.h"
#include <tiffio.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace tiff_io {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write a decoded gray/RGB/RGBA strip row into the RGBA output buffer.
static void expand_strip_rows(const uint8_t* strip_buf, uint8_t* out_pixels,
                               uint32_t w, uint32_t row0, uint32_t row1,
                               uint16_t photometric, uint16_t spp) {
    const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                          photometric == PHOTOMETRIC_MINISWHITE);
    for (uint32_t r = row0; r < row1; ++r) {
        uint8_t* dst = out_pixels + r * w * 4;
        if (is_gray) {
            const bool invert = (photometric == PHOTOMETRIC_MINISWHITE);
            const uint8_t* src = strip_buf + (r - row0) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = invert ? static_cast<uint8_t>(255 - src[x]) : src[x];
                dst[x*4+0] = v; dst[x*4+1] = v;
                dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        } else if (spp == 3) {
            const uint8_t* src = strip_buf + (r - row0) * w * 3;
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*4+0] = src[x*3+0];
                dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2];
                dst[x*4+3] = 255;
            }
        } else { // spp == 4
            const uint8_t* src = strip_buf + (r - row0) * w * 4;
            std::memcpy(dst, src, w * 4);
        }
    }
}

// ---------------------------------------------------------------------------
// Fast path: 8-bit stripped (gray / RGB / RGBA), parallel
// ---------------------------------------------------------------------------

static bool read_strips_8bit(const std::string& /*path*/, TIFF* tif,
                              uint32_t w, uint32_t h,
                              uint16_t photometric, uint16_t spp,
                              image_data& out,
                              std::atomic<float>* progress,
                              const std::atomic<bool>* cancel) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t num_strips  = TIFFNumberOfStrips(tif);
    const tmsize_t strip_bytes = TIFFStripSize(tif);
    std::vector<uint8_t> buf(static_cast<size_t>(strip_bytes));

    for (uint32_t s = 0; s < num_strips; ++s) {
        if (cancel && cancel->load()) return false;
        if (TIFFReadEncodedStrip(tif, s, buf.data(), strip_bytes) < 0)
            return false;

        const uint32_t row0 = s * rows_per_strip;
        const uint32_t row1 = std::min(row0 + rows_per_strip, h);
        expand_strip_rows(buf.data(), out.pixels.data(), w, row0, row1, photometric, spp);

        if (progress) progress->store(static_cast<float>(s + 1) / num_strips);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fast path: 8-bit tiled (gray / RGB / RGBA), parallel
// ---------------------------------------------------------------------------

static void expand_tile_rows(const uint8_t* tile_buf, uint8_t* out_pixels,
                              uint32_t img_w, uint32_t tx, uint32_t ty,
                              uint32_t tile_w, uint32_t copy_w, uint32_t copy_h,
                              uint16_t photometric, uint16_t spp) {
    const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                          photometric == PHOTOMETRIC_MINISWHITE);
    for (uint32_t r = 0; r < copy_h; ++r) {
        uint8_t* dst = out_pixels + (ty + r) * img_w * 4 + tx * 4;
        if (is_gray) {
            const bool invert = (photometric == PHOTOMETRIC_MINISWHITE);
            const uint8_t* src = tile_buf + r * tile_w;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint8_t v = invert ? static_cast<uint8_t>(255 - src[x]) : src[x];
                dst[x*4+0] = v; dst[x*4+1] = v;
                dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        } else if (spp == 3) {
            const uint8_t* src = tile_buf + r * tile_w * 3;
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst[x*4+0] = src[x*3+0];
                dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2];
                dst[x*4+3] = 255;
            }
        } else { // spp == 4
            const uint8_t* src = tile_buf + r * tile_w * 4;
            std::memcpy(dst, src, copy_w * 4);
        }
    }
}

static bool read_tiles_8bit(const std::string& /*path*/, TIFF* tif,
                             uint32_t w, uint32_t h,
                             uint16_t photometric, uint16_t spp,
                             image_data& out,
                             std::atomic<float>* progress,
                             const std::atomic<bool>* cancel) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles     = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x    = (w + tile_w - 1) / tile_w;
    const tmsize_t tile_bytes = TIFFTileSize(tif);
    std::vector<uint8_t> buf(static_cast<size_t>(tile_bytes));

    for (uint32_t t = 0; t < ntiles; ++t) {
        if (cancel && cancel->load()) return false;
        if (TIFFReadEncodedTile(tif, t, buf.data(), tile_bytes) < 0)
            return false;

        const uint32_t tx     = (t % tiles_x) * tile_w;
        const uint32_t ty     = (t / tiles_x) * tile_h;
        const uint32_t copy_w = std::min(tile_w, w - tx);
        const uint32_t copy_h = std::min(tile_h, h - ty);
        expand_tile_rows(buf.data(), out.pixels.data(), w, tx, ty,
                         tile_w, copy_w, copy_h, photometric, spp);

        if (progress) progress->store(static_cast<float>(t + 1) / ntiles);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Generic path: any format via TIFFReadRGBAStrip / TIFFReadRGBATile, parallel
// ---------------------------------------------------------------------------

static bool read_strips_generic(const std::string& /*path*/, TIFF* tif,
                                 uint32_t w, uint32_t h,
                                 image_data& out,
                                 std::atomic<float>* progress,
                                 const std::atomic<bool>* cancel) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t num_strips = TIFFNumberOfStrips(tif);
    std::vector<uint32_t> rgba(static_cast<size_t>(w) * rows_per_strip);

    for (uint32_t s = 0; s < num_strips; ++s) {
        if (cancel && cancel->load()) return false;
        const uint32_t row0 = s * rows_per_strip;
        if (!TIFFReadRGBAStrip(tif, row0, rgba.data()))
            return false;

        const uint32_t row1 = std::min(row0 + rows_per_strip, h);
        const uint32_t rows = row1 - row0;

        // TIFFReadRGBAStrip stores rows bottom-up within the strip; flip to top-down.
        for (uint32_t r = 0; r < rows; ++r) {
            const uint32_t* src = rgba.data() + (rows - 1 - r) * w;
            uint8_t* dst = out.pixels.data() + (row0 + r) * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*4+0] = TIFFGetR(src[x]);
                dst[x*4+1] = TIFFGetG(src[x]);
                dst[x*4+2] = TIFFGetB(src[x]);
                dst[x*4+3] = TIFFGetA(src[x]);
            }
        }

        if (progress) progress->store(static_cast<float>(s + 1) / num_strips);
    }
    return true;
}

static bool read_tiles_generic(const std::string& path, TIFF* tif,
                                uint32_t w, uint32_t h,
                                image_data& out,
                                std::atomic<float>* progress,
                                const std::atomic<bool>* cancel) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles  = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x = (w + tile_w - 1) / tile_w;
    std::vector<uint32_t> rgba(static_cast<size_t>(tile_w) * tile_h);

    for (uint32_t t = 0; t < ntiles; ++t) {
        if (cancel && cancel->load()) return false;
        const uint32_t tx = (t % tiles_x) * tile_w;
        const uint32_t ty = (t / tiles_x) * tile_h;

        if (!TIFFReadRGBATile(tif, tx, ty, rgba.data()))
            return false;

        const uint32_t copy_w = std::min(tile_w, w - tx);
        const uint32_t copy_h = std::min(tile_h, h - ty);

        // TIFFReadRGBATile stores rows bottom-up within the tile; flip to top-down.
        for (uint32_t r = 0; r < copy_h; ++r) {
            const uint32_t* src = rgba.data() + (tile_h - 1 - r) * tile_w;
            uint8_t* dst = out.pixels.data() + (ty + r) * w * 4 + tx * 4;
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst[x*4+0] = TIFFGetR(src[x]);
                dst[x*4+1] = TIFFGetG(src[x]);
                dst[x*4+2] = TIFFGetB(src[x]);
                dst[x*4+3] = TIFFGetA(src[x]);
            }
        }

        if (progress) progress->store(static_cast<float>(t + 1) / ntiles);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 16-bit grayscale paths: read raw uint16 data, auto-scale to 8-bit.
// Required because TIFFReadRGBAStrip maps 16-bit values by v*255/65535,
// which makes 12-bit camera data (0-4095) render as near-black (0-15).
// ---------------------------------------------------------------------------

static bool read_strips_gray16(const std::string& path, TIFF* tif,
                                uint32_t w, uint32_t h,
                                uint16_t photometric,
                                image_data& out,
                                std::atomic<float>* progress,
                                const std::atomic<bool>* cancel) {
    (void)path; // uses the already-open tif handle
    uint32_t rps = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
    if (rps == 0) rps = h;
    const uint32_t num_strips = TIFFNumberOfStrips(tif);
    const tmsize_t sbytes     = TIFFStripSize(tif);

    // Read all strips into a raw uint16 buffer.
    std::vector<uint16_t> raw(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t>  buf(static_cast<size_t>(sbytes));
    for (uint32_t s = 0; s < num_strips; ++s) {
        if (cancel && cancel->load()) return false;
        if (TIFFReadEncodedStrip(tif, s, buf.data(), sbytes) < 0) return false;
        const uint32_t row0 = s * rps;
        const uint32_t row1 = std::min(row0 + rps, h);
        for (uint32_t r = row0; r < row1; ++r) {
            const auto* src = reinterpret_cast<const uint16_t*>(buf.data()) + (r - row0) * w;
            std::memcpy(raw.data() + r * w, src, w * sizeof(uint16_t));
        }
        if (progress) progress->store(static_cast<float>(s + 1) / num_strips * 0.8f);
    }

    // Find the actual maximum to auto-scale (handles 10/12/14/16-bit cameras).
    uint16_t max_val = 1;
    for (uint16_t v : raw) if (v > max_val) max_val = v;

    const bool  invert = (photometric == PHOTOMETRIC_MINISWHITE);
    const float scale  = 255.0f / static_cast<float>(max_val);
    for (size_t i = 0; i < raw.size(); ++i) {
        uint8_t v = static_cast<uint8_t>(static_cast<float>(raw[i]) * scale + 0.5f);
        if (invert) v = static_cast<uint8_t>(255 - v);
        out.pixels[i * 4 + 0] = v;
        out.pixels[i * 4 + 1] = v;
        out.pixels[i * 4 + 2] = v;
        out.pixels[i * 4 + 3] = 255;
    }
    if (progress) progress->store(1.0f);
    return true;
}

static bool read_tiles_gray16(const std::string& path, TIFF* tif,
                               uint32_t w, uint32_t h,
                               uint16_t photometric,
                               image_data& out,
                               std::atomic<float>* progress,
                               const std::atomic<bool>* cancel) {
    (void)path;
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles     = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x    = (w + tile_w - 1) / tile_w;
    const tmsize_t tile_bytes = TIFFTileSize(tif);

    std::vector<uint16_t> raw(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t>  buf(static_cast<size_t>(tile_bytes));
    for (uint32_t t = 0; t < ntiles; ++t) {
        if (cancel && cancel->load()) return false;
        if (TIFFReadEncodedTile(tif, t, buf.data(), tile_bytes) < 0) return false;
        const uint32_t tx     = (t % tiles_x) * tile_w;
        const uint32_t ty     = (t / tiles_x) * tile_h;
        const uint32_t copy_w = std::min(tile_w, w - tx);
        const uint32_t copy_h = std::min(tile_h, h - ty);
        for (uint32_t r = 0; r < copy_h; ++r) {
            const auto* src = reinterpret_cast<const uint16_t*>(buf.data()) + r * tile_w;
            uint16_t*   dst = raw.data() + (ty + r) * w + tx;
            std::memcpy(dst, src, copy_w * sizeof(uint16_t));
        }
        if (progress) progress->store(static_cast<float>(t + 1) / ntiles * 0.8f);
    }

    uint16_t max_val = 1;
    for (uint16_t v : raw) if (v > max_val) max_val = v;

    const bool  invert = (photometric == PHOTOMETRIC_MINISWHITE);
    const float scale  = 255.0f / static_cast<float>(max_val);
    for (size_t i = 0; i < raw.size(); ++i) {
        uint8_t v = static_cast<uint8_t>(static_cast<float>(raw[i]) * scale + 0.5f);
        if (invert) v = static_cast<uint8_t>(255 - v);
        out.pixels[i * 4 + 0] = v;
        out.pixels[i * 4 + 1] = v;
        out.pixels[i * 4 + 2] = v;
        out.pixels[i * 4 + 3] = 255;
    }
    if (progress) progress->store(1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool read(const std::string& path, image_data& out,
          std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
        fprintf(stderr, "tiff_io::read: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) { TIFFClose(tif); return false; }

    uint16_t photometric      = PHOTOMETRIC_RGB;
    uint16_t bits_per_sample  = 8;
    uint16_t samples_per_pixel = 1;
    uint16_t planar_config    = PLANARCONFIG_CONTIG;
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,     &photometric);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE,   &bits_per_sample);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG,    &planar_config);

    out.width  = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    const bool is_tiled  = TIFFIsTiled(tif) != 0;
    const bool is_8bit   = (bits_per_sample == 8);
    const bool is_contig = (planar_config == PLANARCONFIG_CONTIG);
    const bool is_gray   = (photometric == PHOTOMETRIC_MINISBLACK ||
                             photometric == PHOTOMETRIC_MINISWHITE);
    // Fast path: 8-bit contiguous gray(1ch) / RGB(3ch) / RGBA(4ch)
    const bool fast_path = is_8bit && is_contig &&
                           ((is_gray && samples_per_pixel == 1) ||
                            (photometric == PHOTOMETRIC_RGB &&
                             (samples_per_pixel == 3 || samples_per_pixel == 4)));

    fprintf(stderr, "[tiff_io] %s: %ux%u photo=%u bps=%u spp=%u planar=%u tiled=%s fast=%s\n",
            path.c_str(), w, h,
            photometric, bits_per_sample, samples_per_pixel, planar_config,
            is_tiled ? "yes" : "no", fast_path ? "yes" : "no");

    bool ok = false;
    if (fast_path) {
        ok = is_tiled
            ? read_tiles_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress, cancel)
            : read_strips_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress, cancel);
    }

    // 16-bit grayscale: read raw uint16 and auto-scale to 8-bit.
    // Must run before the generic fallback because TIFFReadRGBAStrip maps 16-bit
    // values with v*255/65535, turning 12-bit camera data (0-4095) into 0-15 (near black).
    if (!ok && bits_per_sample == 16 && is_gray && samples_per_pixel == 1 && is_contig) {
        ok = is_tiled
            ? read_tiles_gray16(path, tif, w, h, photometric, out, progress, cancel)
            : read_strips_gray16(path, tif, w, h, photometric, out, progress, cancel);
    }

    // Generic fallback: any format, any bit depth (1/2/4/16-bit, palette, CMYK, …)
    if (!ok && !(cancel && cancel->load())) {
        ok = is_tiled
            ? read_tiles_generic(path, tif, w, h, out, progress, cancel)
            : read_strips_generic(path, tif, w, h, out, progress, cancel);
    }

    if (!ok) {
        fprintf(stderr, "tiff_io::read: failed to decode '%s'\n", path.c_str());
    } else {
        const uint8_t* p = out.pixels.data();
        // Print first 4 pixel RGBA values to verify data is non-zero
        fprintf(stderr, "[tiff_io] pixels[0..3] RGBA: (%u,%u,%u,%u) (%u,%u,%u,%u) (%u,%u,%u,%u) (%u,%u,%u,%u)\n",
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
    }

    TIFFClose(tif);
    return ok;
}

bool write(const std::string& path, const image_data& img) {
    if (img.empty()) return false;

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) {
        fprintf(stderr, "tiff_io::write: cannot open '%s' for writing\n", path.c_str());
        return false;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      static_cast<uint32_t>(img.width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     static_cast<uint32_t>(img.height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(4));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);

    uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);

    const uint32_t row_stride = static_cast<uint32_t>(img.width) * 4;
    const uint32_t rps = TIFFDefaultStripSize(tif, row_stride);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rps);

    for (int row = 0; row < img.height; ++row) {
        void* row_ptr = const_cast<uint8_t*>(img.pixels.data() + static_cast<size_t>(row) * row_stride);
        if (TIFFWriteScanline(tif, row_ptr, static_cast<uint32_t>(row), 0) < 0) {
            fprintf(stderr, "tiff_io::write: write error at row %d\n", row);
            TIFFClose(tif);
            return false;
        }
    }

    TIFFClose(tif);
    return true;
}

} // namespace tiff_io

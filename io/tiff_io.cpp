#include "io/tiff_io.h"
#include <tiffio.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace tiff_io {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Number of worker threads: hardware concurrency, capped at 8.
static int worker_count(uint32_t units) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(1, std::min({hw, 8, static_cast<int>(units)}));
}

// Run worker(thread_id) on nthreads threads (or inline when nthreads == 1).
template<typename Fn>
static void parallel_for(int nthreads, Fn worker) {
    if (nthreads == 1) {
        worker(0);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t)
        threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
}

// Write a decoded gray/RGB/RGBA strip row into the RGBA output buffer.
static void expand_strip_rows(const uint8_t* strip_buf, uint8_t* out_pixels,
                               uint32_t w, uint32_t row0, uint32_t row1,
                               uint16_t photometric, uint16_t spp) {
    const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                          photometric == PHOTOMETRIC_MINISWHITE);
    for (uint32_t r = row0; r < row1; ++r) {
        uint8_t* dst = out_pixels + r * w * 4;
        if (is_gray) {
            const uint8_t* src = strip_buf + (r - row0) * w;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = src[x];
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

static bool read_strips_8bit(const std::string& path, TIFF* tif,
                              uint32_t w, uint32_t h,
                              uint16_t photometric, uint16_t spp,
                              image_data& out,
                              std::atomic<float>* progress) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t  num_strips  = TIFFNumberOfStrips(tif);
    const tmsize_t  strip_bytes = TIFFStripSize(tif);
    const int       nthreads    = worker_count(num_strips);

    std::atomic<uint32_t> strips_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(strip_bytes);

        for (uint32_t s = static_cast<uint32_t>(tid); s < num_strips; s += nthreads) {
            if (had_error.load()) break;

            if (TIFFReadEncodedStrip(ltif, s, buf.data(), strip_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t row0 = s * rows_per_strip;
            const uint32_t row1 = std::min(row0 + rows_per_strip, h);
            expand_strip_rows(buf.data(), out.pixels.data(), w, row0, row1, photometric, spp);

            const uint32_t done = ++strips_done;
            if (progress) progress->store(static_cast<float>(done) / num_strips);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
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
            const uint8_t* src = tile_buf + r * tile_w;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint8_t v = src[x];
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

static bool read_tiles_8bit(const std::string& path, TIFF* tif,
                             uint32_t w, uint32_t h,
                             uint16_t photometric, uint16_t spp,
                             image_data& out,
                             std::atomic<float>* progress) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles   = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x  = (w + tile_w - 1) / tile_w;
    const tmsize_t tile_bytes = TIFFTileSize(tif);
    const int      nthreads  = worker_count(ntiles);

    std::atomic<uint32_t> tiles_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(tile_bytes);

        for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
            if (had_error.load()) break;

            const uint32_t tx = (t % tiles_x) * tile_w;
            const uint32_t ty = (t / tiles_x) * tile_h;

            if (TIFFReadEncodedTile(ltif, t, buf.data(), tile_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t copy_w = std::min(tile_w, w - tx);
            const uint32_t copy_h = std::min(tile_h, h - ty);
            expand_tile_rows(buf.data(), out.pixels.data(), w, tx, ty,
                             tile_w, copy_w, copy_h, photometric, spp);

            const uint32_t done = ++tiles_done;
            if (progress) progress->store(static_cast<float>(done) / ntiles);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

// ---------------------------------------------------------------------------
// Generic path: any format via TIFFReadRGBAStrip / TIFFReadRGBATile, parallel
// ---------------------------------------------------------------------------

static bool read_strips_generic(const std::string& path, TIFF* tif,
                                 uint32_t w, uint32_t h,
                                 image_data& out,
                                 std::atomic<float>* progress) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t num_strips = TIFFNumberOfStrips(tif);
    const int      nthreads   = worker_count(num_strips);

    std::atomic<uint32_t> strips_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint32_t> rgba(static_cast<size_t>(w) * rows_per_strip);

        for (uint32_t s = static_cast<uint32_t>(tid); s < num_strips; s += nthreads) {
            if (had_error.load()) break;

            const uint32_t row0 = s * rows_per_strip;
            if (!TIFFReadRGBAStrip(ltif, row0, rgba.data())) {
                had_error.store(true); break;
            }

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

            const uint32_t done = ++strips_done;
            if (progress) progress->store(static_cast<float>(done) / num_strips);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

static bool read_tiles_generic(const std::string& path, TIFF* tif,
                                uint32_t w, uint32_t h,
                                image_data& out,
                                std::atomic<float>* progress) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles  = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x = (w + tile_w - 1) / tile_w;
    const int      nthreads = worker_count(ntiles);

    std::atomic<uint32_t> tiles_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint32_t> rgba(static_cast<size_t>(tile_w) * tile_h);

        for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
            if (had_error.load()) break;

            const uint32_t tx = (t % tiles_x) * tile_w;
            const uint32_t ty = (t / tiles_x) * tile_h;

            if (!TIFFReadRGBATile(ltif, tx, ty, rgba.data())) {
                had_error.store(true); break;
            }

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

            const uint32_t done = ++tiles_done;
            if (progress) progress->store(static_cast<float>(done) / ntiles);
        }

        TIFFClose(ltif);
    });

    return !had_error.load();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool read(const std::string& path, image_data& out, std::atomic<float>* progress) {
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

    bool ok = false;
    if (fast_path) {
        ok = is_tiled
            ? read_tiles_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress)
            : read_strips_8bit(path, tif, w, h, photometric, samples_per_pixel, out, progress);
    }

    // Generic fallback: any format, any bit depth (1/2/4/16-bit, palette, CMYK, …)
    if (!ok) {
        ok = is_tiled
            ? read_tiles_generic(path, tif, w, h, out, progress)
            : read_strips_generic(path, tif, w, h, out, progress);
    }

    if (!ok)
        fprintf(stderr, "tiff_io::read: failed to decode '%s'\n", path.c_str());

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

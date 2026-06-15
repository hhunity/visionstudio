#include "io/tiff_io.h"
#include <tiffio.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace tiff_io {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Number of worker threads.
// max_threads == 0: use full hardware concurrency (no artificial cap).
// max_threads  > 0: use at most that many threads.
static int worker_count(uint32_t units, int max_threads = 0) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int cap = (max_threads > 0) ? std::min(hw, max_threads) : hw;
    return std::max(1, std::min(cap, static_cast<int>(units)));
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

// Expand a decoded strip into the RGBA output buffer.
static void expand_strip_rows(const uint8_t* strip_buf,
                               uint32_t w, uint32_t row0, uint32_t row1,
                               PixelFormat src_fmt,
                               uint8_t* out_pixels) {
    const uint32_t nrows = row1 - row0;
    if (src_fmt == PixelFormat::rgba) {
        std::memcpy(out_pixels + static_cast<size_t>(row0) * w * 4, strip_buf,
                    static_cast<size_t>(nrows) * w * 4);
    } else if (src_fmt == PixelFormat::gray) {
        for (uint32_t r = 0; r < nrows; ++r) {
            const uint8_t* src = strip_buf + static_cast<size_t>(r) * w;
            uint8_t*       dst = out_pixels + static_cast<size_t>(row0 + r) * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = src[x];
                dst[x*4+0] = v; dst[x*4+1] = v; dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        }
    } else { // rgb
        for (uint32_t r = 0; r < nrows; ++r) {
            const uint8_t* src = strip_buf + static_cast<size_t>(r) * w * 3;
            uint8_t*       dst = out_pixels + static_cast<size_t>(row0 + r) * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                dst[x*4+0] = src[x*3+0]; dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2]; dst[x*4+3] = 255;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Strip reader: 8/16-bit contiguous gray / RGB / RGBA, parallel
// ---------------------------------------------------------------------------

static bool read_strips(const std::string& path, TIFF* tif,
                        uint32_t w, uint32_t h,
                        PixelFormat src_fmt, PixelFormat dst_fmt,
                        image_data& out,
                        int max_threads,
                        std::atomic<float>* progress) {
    uint32_t rows_per_strip = h;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
    if (rows_per_strip == 0) rows_per_strip = h;

    const uint32_t  num_strips  = TIFFNumberOfStrips(tif);
    const tmsize_t  strip_bytes = TIFFStripSize(tif);
    if (strip_bytes <= 0) {
        fprintf(stderr, "read_strips: invalid strip size (internal overflow or empty strip)\n");
        return false;
    }
    const int       nthreads    = worker_count(num_strips, max_threads);

    std::atomic<uint32_t> strips_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(static_cast<size_t>(strip_bytes));

        for (uint32_t s = static_cast<uint32_t>(tid); s < num_strips; s += nthreads) {
            if (had_error.load()) break;

            if (TIFFReadEncodedStrip(ltif, s, buf.data(), strip_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t row0 = s * rows_per_strip;
            const uint32_t row1 = std::min(row0 + rows_per_strip, h);
            if (src_fmt == dst_fmt && src_fmt == PixelFormat::gray) {
                std::memcpy(out.pixels.data() + static_cast<size_t>(row0) * w, buf.data(),
                            static_cast<size_t>(row1 - row0) * w);
            } else if (src_fmt == dst_fmt && src_fmt == PixelFormat::rgb) {
                std::memcpy(out.pixels.data() + static_cast<size_t>(row0) * w * 3, buf.data(),
                            static_cast<size_t>(row1 - row0) * w * 3);
            } else {
                expand_strip_rows(buf.data(), w, row0, row1, src_fmt, out.pixels.data());
            }

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

// Expand a decoded tile into the RGBA output buffer.
static void expand_tile_rows(const uint8_t* tile_buf,
                              uint32_t img_w, uint32_t tx, uint32_t ty,
                              uint32_t tile_w, uint32_t copy_w, uint32_t copy_h,
                              PixelFormat src_fmt,
                              uint8_t* out_pixels) {
    if (src_fmt == PixelFormat::gray) {
        for (uint32_t r = 0; r < copy_h; ++r) {
            const uint8_t* src = tile_buf + static_cast<size_t>(r) * tile_w;
            uint8_t*       dst = out_pixels + static_cast<size_t>(ty + r) * img_w * 4 + static_cast<size_t>(tx) * 4;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint8_t v = src[x];
                dst[x*4+0] = v; dst[x*4+1] = v; dst[x*4+2] = v; dst[x*4+3] = 255;
            }
        }
    } else if (src_fmt == PixelFormat::rgb) {
        for (uint32_t r = 0; r < copy_h; ++r) {
            const uint8_t* src = tile_buf + static_cast<size_t>(r) * tile_w * 3;
            uint8_t*       dst = out_pixels + static_cast<size_t>(ty + r) * img_w * 4 + static_cast<size_t>(tx) * 4;
            for (uint32_t x = 0; x < copy_w; ++x) {
                dst[x*4+0] = src[x*3+0]; dst[x*4+1] = src[x*3+1];
                dst[x*4+2] = src[x*3+2]; dst[x*4+3] = 255;
            }
        }
    } else { // rgba
        for (uint32_t r = 0; r < copy_h; ++r) {
            const uint8_t* src = tile_buf + static_cast<size_t>(r) * tile_w * 4;
            uint8_t*       dst = out_pixels + static_cast<size_t>(ty + r) * img_w * 4 + static_cast<size_t>(tx) * 4;
            std::memcpy(dst, src, static_cast<size_t>(copy_w) * 4);
        }
    }
}

static bool read_tiles(const std::string& path, TIFF* tif,
                       uint32_t w, uint32_t h,
                       PixelFormat src_fmt, PixelFormat dst_fmt,
                       image_data& out,
                       int max_threads,
                       std::atomic<float>* progress) {
    uint32_t tile_w = 0, tile_h = 0;
    TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &tile_w);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
    if (tile_w == 0 || tile_h == 0) return false;

    const uint32_t ntiles   = TIFFNumberOfTiles(tif);
    const uint32_t tiles_x  = (w + tile_w - 1) / tile_w;
    const tmsize_t tile_bytes = TIFFTileSize(tif);
    if (tile_bytes <= 0) {
        fprintf(stderr, "read_tiles: invalid tile size (internal overflow or empty tile)\n");
        return false;
    }
    const int      nthreads  = worker_count(ntiles, max_threads);

    std::atomic<uint32_t> tiles_done{0};
    std::atomic<bool>     had_error{false};

    parallel_for(nthreads, [&](int tid) {
        TIFF* ltif = TIFFOpen(path.c_str(), "r");
        if (!ltif) { had_error.store(true); return; }

        std::vector<uint8_t> buf(static_cast<size_t>(tile_bytes));

        for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
            if (had_error.load()) break;

            const uint32_t tx = (t % tiles_x) * tile_w;
            const uint32_t ty = (t / tiles_x) * tile_h;

            if (TIFFReadEncodedTile(ltif, t, buf.data(), tile_bytes) < 0) {
                had_error.store(true); break;
            }

            const uint32_t copy_w = std::min(tile_w, w - tx);
            const uint32_t copy_h = std::min(tile_h, h - ty);
            if (src_fmt == dst_fmt && src_fmt == PixelFormat::gray) {
                for (uint32_t r = 0; r < copy_h; ++r) {
                    std::memcpy(out.pixels.data() + static_cast<size_t>(ty + r) * w + tx,
                                buf.data() + static_cast<size_t>(r) * tile_w, copy_w);
                }
            } else if (src_fmt == dst_fmt && src_fmt == PixelFormat::rgb) {
                for (uint32_t r = 0; r < copy_h; ++r) {
                    std::memcpy(out.pixels.data() + (static_cast<size_t>(ty + r) * w + tx) * 3,
                                buf.data() + static_cast<size_t>(r) * tile_w * 3,
                                static_cast<size_t>(copy_w) * 3);
                }
            } else {
                expand_tile_rows(buf.data(), w, tx, ty,
                                 tile_w, copy_w, copy_h, src_fmt, out.pixels.data());
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

PixelFormat detect_format(const std::string& path) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) return PixelFormat::rgba;
    uint16_t photometric      = PHOTOMETRIC_RGB;
    uint16_t samples_per_pixel = 3;
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,     &photometric);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    TIFFClose(tif);
    if (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE)
        return PixelFormat::gray;
    return (samples_per_pixel == 3) ? PixelFormat::rgb : PixelFormat::rgba;
}

bool read(const std::string& path, image_data& out, std::atomic<float>* progress,
          const ReadOptions& opts) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
        fprintf(stderr, "tiff_io::read: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0) { TIFFClose(tif); return false; }
    if (w > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        h > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        fprintf(stderr, "tiff_io::read: image dimensions too large for int (%ux%u)\n", w, h);
        TIFFClose(tif);
        return false;
    }

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

    const bool is_tiled  = TIFFIsTiled(tif) != 0;
    const bool is_contig = (planar_config == PLANARCONFIG_CONTIG);
    // Map TIFF tags to PixelFormat for source.
    const PixelFormat src_fmt =
        (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE)
            ? PixelFormat::gray
            : (samples_per_pixel == 3 ? PixelFormat::rgb : PixelFormat::rgba);

    const bool supported = (bits_per_sample == 8) && is_contig &&
                           (src_fmt == PixelFormat::gray ||
                            src_fmt == PixelFormat::rgb  ||
                            src_fmt == PixelFormat::rgba);

    if (!supported) {
        fprintf(stderr, "tiff_io::read: unsupported format in '%s' (bps=%u spp=%u photometric=%u planar=%u)\n",
                path.c_str(), bits_per_sample, samples_per_pixel, photometric, planar_config);
        TIFFClose(tif);
        return false;
    }

    const PixelFormat dst_fmt = opts.output_format;

    // Direct decode when source and destination formats match (no RGBA intermediate).
    const bool direct = (src_fmt == dst_fmt) &&
                        (src_fmt == PixelFormat::gray || src_fmt == PixelFormat::rgb);
    const uint32_t out_ch = direct ? static_cast<uint32_t>(src_fmt == PixelFormat::gray ? 1 : 3)
                                   : 4u;
    out.pixels.resize(static_cast<size_t>(w) * h * out_ch);

    const int mt = opts.max_threads;
    const bool ok = is_tiled
        ? read_tiles(path, tif, w, h, src_fmt, dst_fmt, out, mt, progress)
        : read_strips(path, tif, w, h, src_fmt, dst_fmt, out, mt, progress);

    if (!ok) {
        fprintf(stderr, "tiff_io::read: failed to decode '%s'\n", path.c_str());
        TIFFClose(tif);
        return false;
    }

    TIFFClose(tif);

    if (direct) {
        out.format = dst_fmt;
    } else if (dst_fmt == PixelFormat::gray) {
        const size_t npixels = static_cast<size_t>(w) * h;
        std::vector<uint8_t> buf(npixels);
        for (size_t i = 0; i < npixels; ++i)
            buf[i] = static_cast<uint8_t>(out.pixels[i*4+0] * 0.299f +
                                          out.pixels[i*4+1] * 0.587f +
                                          out.pixels[i*4+2] * 0.114f + 0.5f);
        out.pixels = std::move(buf);
        out.format = PixelFormat::gray;
    } else if (dst_fmt == PixelFormat::rgb) {
        const size_t npixels = static_cast<size_t>(w) * h;
        std::vector<uint8_t> buf(npixels * 3);
        for (size_t i = 0; i < npixels; ++i) {
            buf[i*3+0] = out.pixels[i*4+0];
            buf[i*3+1] = out.pixels[i*4+1];
            buf[i*3+2] = out.pixels[i*4+2];
        }
        out.pixels = std::move(buf);
        out.format = PixelFormat::rgb;
    } else {
        out.format = PixelFormat::rgba;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pixel conversion helper used by both tile and strip write paths.
// Converts a rectangular region of img into a contiguous raw buffer.
// For tiled layout, row_stride is ts (tile width); for strip layout, row_stride is w.
// ---------------------------------------------------------------------------
static void convert_pixels(const image_data& img,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t copy_w, uint32_t copy_h,
                            uint32_t w, uint32_t row_stride,
                            PixelFormat dst_fmt,
                            uint8_t* raw) {
    const int src_ch = img.channels();
    const int dst_ch = (dst_fmt == PixelFormat::gray) ? 1
                     : (dst_fmt == PixelFormat::rgb)  ? 3 : 4;

    for (uint32_t r = 0; r < copy_h; ++r) {
        const uint8_t* src = img.pixels.data() + (static_cast<size_t>(src_y + r) * w + src_x) * src_ch;
        uint8_t*       dst = raw + static_cast<size_t>(r) * row_stride * dst_ch;

        if (dst_ch == 1) {
            if (src_ch == 1) {
                std::memcpy(dst, src, copy_w);
            } else {
                for (uint32_t x = 0; x < copy_w; ++x)
                    dst[x] = static_cast<uint8_t>(src[x*src_ch+0] * 0.299f +
                                                  src[x*src_ch+1] * 0.587f +
                                                  src[x*src_ch+2] * 0.114f + 0.5f);
            }
        } else if (dst_ch == 3) {
            if (src_ch == 3) {
                std::memcpy(dst, src, copy_w * 3);
            } else if (src_ch == 1) {
                for (uint32_t x = 0; x < copy_w; ++x) {
                    const uint8_t v = src[x];
                    dst[x*3+0] = v; dst[x*3+1] = v; dst[x*3+2] = v;
                }
            } else { // 4 → 3: strip alpha
                for (uint32_t x = 0; x < copy_w; ++x) {
                    dst[x*3+0] = src[x*4+0];
                    dst[x*3+1] = src[x*4+1];
                    dst[x*3+2] = src[x*4+2];
                }
            }
        } else { // dst_ch == 4
            if (src_ch == 4) {
                std::memcpy(dst, src, copy_w * 4);
            } else if (src_ch == 1) {
                // Pack to 0xFFRRGGBB (little-endian) for faster 4-byte stores.
                uint32_t* d32 = reinterpret_cast<uint32_t*>(dst);
                for (uint32_t x = 0; x < copy_w; ++x) {
                    const uint8_t v = src[x];
                    d32[x] = 0xFF000000u | (static_cast<uint32_t>(v) * 0x00010101u);
                }
            } else { // 3 → 4: add alpha
                uint32_t* d32 = reinterpret_cast<uint32_t*>(dst);
                for (uint32_t x = 0; x < copy_w; ++x) {
                    const uint8_t r = src[x*3+0];
                    const uint8_t g = src[x*3+1];
                    const uint8_t b = src[x*3+2];
                    d32[x] = 0xFF000000u
                           | (static_cast<uint32_t>(b) << 16)
                           | (static_cast<uint32_t>(g) << 8)
                           |  static_cast<uint32_t>(r);
                }
            }
        }
    }
}

bool write(const std::string& path, const image_data& img,
           std::atomic<float>* progress, const WriteOptions& opts) {
    if (img.empty()) return false;

    const uint32_t ts = opts.tile_size;
    if (ts > 0 && (ts < 16 || ts % 16 != 0)) {
        fprintf(stderr, "tiff_io::write: tile_size must be 0 (strip) or a multiple of 16 (got %u)\n", ts);
        return false;
    }

    const uint32_t    w       = static_cast<uint32_t>(img.width);
    const uint32_t    h       = static_cast<uint32_t>(img.height);
    PixelFormat dst_fmt = opts.output_format;

    const uint32_t    spp     = (dst_fmt == PixelFormat::gray) ? 1u
                              : (dst_fmt == PixelFormat::rgb)  ? 3u : 4u;

    // Use BigTIFF (64-bit file offsets) when the uncompressed output would
    // exceed the 4 GB limit of classic TIFF (32-bit offsets).  Without this,
    // strip offsets beyond 4 GB are silently truncated, producing a corrupt
    // file whose pixels appear black (or repeat) past the 2^32-byte boundary.
    const uint64_t est_bytes = static_cast<uint64_t>(w) * h * spp;
    const bool     bigtiff   = (est_bytes > static_cast<uint64_t>(UINT32_MAX));
    TIFF* tif = TIFFOpen(path.c_str(), bigtiff ? "w8" : "w");
    if (!tif) {
        fprintf(stderr, "tiff_io::write: cannot open '%s' for writing\n", path.c_str());
        return false;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(spp));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     (dst_fmt == PixelFormat::gray)
                                                   ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);

    uint16_t compression_tag = COMPRESSION_NONE;
    switch (opts.compression) {
        case WriteOptions::Compression::none:     compression_tag = COMPRESSION_NONE; break;
        case WriteOptions::Compression::deflate:  compression_tag = COMPRESSION_ADOBE_DEFLATE; break;
        case WriteOptions::Compression::lzw:      compression_tag = COMPRESSION_LZW; break;
        case WriteOptions::Compression::packbits: compression_tag = COMPRESSION_PACKBITS; break;
        case WriteOptions::Compression::zstd:
#ifdef COMPRESSION_ZSTD
            compression_tag = COMPRESSION_ZSTD;
#else
            compression_tag = COMPRESSION_ADOBE_DEFLATE; // fallback if zstd is unavailable
#endif
            break;
    }
    TIFFSetField(tif, TIFFTAG_COMPRESSION, compression_tag);
    if (opts.compression == WriteOptions::Compression::deflate) {
        const int lvl = std::clamp(opts.compression_level, 0, 9);
        TIFFSetField(tif, TIFFTAG_ZIPQUALITY, static_cast<uint16_t>(lvl));
    }

    if (dst_fmt == PixelFormat::rgba) {
        uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);
    }

    // Compressed segment buffer (used only when compression_level > 0).
    struct SegBuf {
        std::vector<uint8_t> data;
        uLongf               size = 0;
        bool                 ok   = false;
    };

    std::atomic<bool> had_error{false};
    const bool manual_deflate = (opts.compression == WriteOptions::Compression::deflate &&
                                 opts.compression_level > 0);

    if (ts > 0) {
        // ----------------------------------------------------------------
        // Tiled layout
        // ----------------------------------------------------------------
        TIFFSetField(tif, TIFFTAG_TILEWIDTH,  ts);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, ts);

        const uint32_t tiles_x = (w + ts - 1) / ts;
        const uint32_t tiles_y = (h + ts - 1) / ts;
        const uint32_t ntiles  = tiles_x * tiles_y;

        if (!manual_deflate) {
            // Use libtiff's encoded path (includes none/LZW/PackBits/ZSTD or deflate level 0).
            std::vector<uint8_t> raw(static_cast<size_t>(ts) * ts * spp);
            for (uint32_t t = 0; t < ntiles; ++t) {
                const uint32_t tx     = (t % tiles_x) * ts;
                const uint32_t ty     = (t / tiles_x) * ts;
                const uint32_t copy_w = std::min(ts, w - tx);
                const uint32_t copy_h = std::min(ts, h - ty);
                // ①: zero-pad only edge tiles
                if (copy_w < ts || copy_h < ts) std::fill(raw.begin(), raw.end(), 0);
                convert_pixels(img, tx, ty, copy_w, copy_h, w, ts, dst_fmt, raw.data());
                if (TIFFWriteEncodedTile(tif, t, raw.data(),
                                         static_cast<tmsize_t>(raw.size())) < 0) {
                    fprintf(stderr, "tiff_io::write: failed to write tile %u\n", t);
                    TIFFClose(tif);
                    return false;
                }
                if (progress) progress->store(static_cast<float>(t + 1) / ntiles);
            }
        } else {
            // Manual deflate: parallel compress, serial write (zlib).
            const int nthreads = worker_count(ntiles, opts.max_threads);
            std::vector<SegBuf> segs(ntiles);

            parallel_for(nthreads, [&](int tid) {
                std::vector<uint8_t> raw(static_cast<size_t>(ts) * ts * spp);
                for (uint32_t t = static_cast<uint32_t>(tid); t < ntiles; t += nthreads) {
                    if (had_error.load()) break;

                    const uint32_t tx     = (t % tiles_x) * ts;
                    const uint32_t ty     = (t / tiles_x) * ts;
                    const uint32_t copy_w = std::min(ts, w - tx);
                    const uint32_t copy_h = std::min(ts, h - ty);
                    // ①: zero-pad only edge tiles
                    if (copy_w < ts || copy_h < ts) std::fill(raw.begin(), raw.end(), 0);
                    convert_pixels(img, tx, ty, copy_w, copy_h, w, ts, dst_fmt, raw.data());

                    const uLong raw_len = static_cast<uLong>(raw.size());
                    segs[t].data.resize(compressBound(raw_len));
                    segs[t].size = static_cast<uLongf>(segs[t].data.size());
                    segs[t].ok = (compress2(segs[t].data.data(), &segs[t].size,
                                            raw.data(), raw_len,
                                            opts.compression_level) == Z_OK);
                    if (!segs[t].ok) had_error.store(true);
                }
            });

            if (had_error.load()) {
                fprintf(stderr, "tiff_io::write: compression failed\n");
                TIFFClose(tif);
                return false;
            }

            for (uint32_t t = 0; t < ntiles; ++t) {
                if (TIFFWriteRawTile(tif, t,
                                     segs[t].data.data(),
                                     static_cast<tmsize_t>(segs[t].size)) < 0) {
                    fprintf(stderr, "tiff_io::write: failed to write tile %u\n", t);
                    TIFFClose(tif);
                    return false;
                }
                if (progress) progress->store(static_cast<float>(t + 1) / ntiles);
            }
        }
    } else {
        // ----------------------------------------------------------------
        // Strip layout (tile_size == 0)
        // ----------------------------------------------------------------
        const uint32_t rps     = (opts.rows_per_strip > 0) ? opts.rows_per_strip : 64u;
        const uint32_t nstrips = (h + rps - 1) / rps;
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rps);

        const bool needs_conversion = (img.format != dst_fmt);

        if (!manual_deflate) {
            // Use libtiff's encoded path (includes none/LZW/PackBits/ZSTD or deflate level 0).
            std::vector<uint8_t> converted;
            if (needs_conversion)
                converted.resize(static_cast<size_t>(w) * rps * spp);

            for (uint32_t s = 0; s < nstrips; ++s) {
                const uint32_t row0   = s * rps;
                const uint32_t copy_h = std::min(rps, h - row0);
                const uLong    raw_len = static_cast<uLong>(static_cast<size_t>(w) * copy_h * spp);

                const uint8_t* src_ptr;
                if (needs_conversion) {
                    convert_pixels(img, 0, row0, w, copy_h, w, w, dst_fmt, converted.data());
                    src_ptr = converted.data();
                } else {
                    src_ptr = img.pixels.data() + static_cast<size_t>(row0) * w * spp;
                }

                if (TIFFWriteEncodedStrip(tif, s, const_cast<uint8_t*>(src_ptr),
                                          static_cast<tmsize_t>(raw_len)) < 0) {
                    fprintf(stderr, "tiff_io::write: failed to write strip %u\n", s);
                    TIFFClose(tif);
                    return false;
                }
                if (progress) progress->store(static_cast<float>(s + 1) / nstrips);
            }
        } else {
            // Manual deflate: parallel compress, serial write (zlib).
            const int nthreads = worker_count(nstrips, opts.max_threads);
            std::vector<SegBuf> segs(nstrips);

            parallel_for(nthreads, [&](int tid) {
                std::vector<uint8_t> converted;
                if (needs_conversion)
                    converted.resize(static_cast<size_t>(w) * rps * spp);

                for (uint32_t s = static_cast<uint32_t>(tid); s < nstrips; s += nthreads) {
                    if (had_error.load()) break;

                    const uint32_t row0   = s * rps;
                    const uint32_t copy_h = std::min(rps, h - row0);
                    const uLong    raw_len = static_cast<uLong>(static_cast<size_t>(w) * copy_h * spp);

                    const uint8_t* src_ptr;
                    if (needs_conversion) {
                        convert_pixels(img, 0, row0, w, copy_h, w, w, dst_fmt, converted.data());
                        src_ptr = converted.data();
                    } else {
                        src_ptr = img.pixels.data() + static_cast<size_t>(row0) * w * spp;
                    }

                    segs[s].data.resize(compressBound(raw_len));
                    segs[s].size = static_cast<uLongf>(segs[s].data.size());
                    segs[s].ok = (compress2(segs[s].data.data(), &segs[s].size,
                                            src_ptr, raw_len,
                                            opts.compression_level) == Z_OK);
                    if (!segs[s].ok) had_error.store(true);
                }
            });

            if (had_error.load()) {
                fprintf(stderr, "tiff_io::write: compression failed\n");
                TIFFClose(tif);
                return false;
            }

            for (uint32_t s = 0; s < nstrips; ++s) {
                if (TIFFWriteRawStrip(tif, s,
                                      segs[s].data.data(),
                                      static_cast<tmsize_t>(segs[s].size)) < 0) {
                    fprintf(stderr, "tiff_io::write: failed to write strip %u\n", s);
                    TIFFClose(tif);
                    return false;
                }
                if (progress) progress->store(static_cast<float>(s + 1) / nstrips);
            }
        }
    }

    TIFFClose(tif);
    return true;
}


} // namespace tiff_io

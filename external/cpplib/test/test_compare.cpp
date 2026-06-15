#include "io/tiff_io.h"
#include <tiffio.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using ms_t = double;

static ms_t now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(
        high_resolution_clock::now().time_since_epoch()).count();
}

static image_data make_gradient(int w, int h)
{
    image_data img;
    img.width  = w;
    img.height = h;
    img.pixels.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.pixels.data() + (y * w + x) * 4;
            p[0] = static_cast<uint8_t>(x & 0xFF);
            p[1] = static_cast<uint8_t>(y & 0xFF);
            p[2] = 128;
            p[3] = 255;
        }
    return img;
}

// ---------------------------------------------------------------------------
// Baseline write: sequential TIFFWriteScanline, no compression (original style)
// ---------------------------------------------------------------------------

static bool write_sequential(const std::string& path, const image_data& img)
{
    if (img.empty()) return false;

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) return false;

    const uint32_t w = static_cast<uint32_t>(img.width);
    const uint32_t h = static_cast<uint32_t>(img.height);

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(4));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     COMPRESSION_NONE);

    uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);

    const uint32_t row_stride = w * 4;
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, row_stride));

    for (int row = 0; row < img.height; ++row) {
        void* ptr = const_cast<uint8_t*>(
            img.pixels.data() + static_cast<size_t>(row) * row_stride);
        if (TIFFWriteScanline(tif, ptr, static_cast<uint32_t>(row), 0) < 0) {
            TIFFClose(tif);
            return false;
        }
    }

    TIFFClose(tif);
    return true;
}

// ---------------------------------------------------------------------------
// Baseline write: sequential TIFFWriteScanline, with DEFLATE compression
// ---------------------------------------------------------------------------

static bool write_sequential_compressed(const std::string& path,
                                        const image_data& img,
                                        int level = 6)
{
    if (img.empty()) return false;

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) return false;

    const uint32_t w = static_cast<uint32_t>(img.width);
    const uint32_t h = static_cast<uint32_t>(img.height);

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(4));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY,      level);

    uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(1), &extra_type);

    const uint32_t row_stride = w * 4;
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, row_stride));

    for (int row = 0; row < img.height; ++row) {
        void* ptr = const_cast<uint8_t*>(
            img.pixels.data() + static_cast<size_t>(row) * row_stride);
        if (TIFFWriteScanline(tif, ptr, static_cast<uint32_t>(row), 0) < 0) {
            TIFFClose(tif);
            return false;
        }
    }

    TIFFClose(tif);
    return true;
}

// ---------------------------------------------------------------------------
// Baseline read: single-threaded (max_threads = 1)
// ---------------------------------------------------------------------------

static bool read_sequential(const std::string& path, image_data& out)
{
    return tiff_io::read(path, out, nullptr, {.max_threads = 1});
}

// ---------------------------------------------------------------------------
// Print one result row
// ---------------------------------------------------------------------------

static void print_row(const char* label, int w, int h,
                      ms_t write_ms, ms_t read_ms)
{
    const double mb = static_cast<double>(w) * h * 4 / (1024.0 * 1024.0);
    std::printf("  %-30s  write %6.1f ms (%5.0f MB/s)  "
                "read %6.1f ms (%5.0f MB/s)\n",
                label,
                write_ms, mb / (write_ms / 1000.0),
                read_ms,  mb / (read_ms  / 1000.0));
}

// ---------------------------------------------------------------------------
// One benchmark round
// ---------------------------------------------------------------------------

static void bench(int w, int h)
{
    const double mb = static_cast<double>(w) * h * 4 / (1024.0 * 1024.0);
    std::printf("\n=== %dx%d  (%.1f MB raw RGBA) ===\n", w, h, mb);

    const image_data src = make_gradient(w, h);
    image_data dst;
    ms_t t0, t1;

    // ---- Sequential write (no compression) ----
    t0 = now_ms();
    write_sequential("_seq.tiff", src);
    t1 = now_ms();
    const ms_t seq_write = t1 - t0;

    t0 = now_ms();
    read_sequential("_seq.tiff", dst);
    t1 = now_ms();
    const ms_t seq_read = t1 - t0;

    print_row("sequential (no compress)", w, h, seq_write, seq_read);

    // ---- Sequential write, DEFLATE level 1 ----
    t0 = now_ms();
    write_sequential_compressed("_seq_l1.tiff", src, 1);
    t1 = now_ms();
    const ms_t seq_l1_write = t1 - t0;

    t0 = now_ms();
    read_sequential("_seq_l1.tiff", dst);
    t1 = now_ms();
    const ms_t seq_l1_read = t1 - t0;

    print_row("sequential (level=1)", w, h, seq_l1_write, seq_l1_read);

    // ---- Sequential write, DEFLATE level 6 ----
    t0 = now_ms();
    write_sequential_compressed("_seq_l6.tiff", src, 6);
    t1 = now_ms();
    const ms_t seq_l6_write = t1 - t0;

    t0 = now_ms();
    read_sequential("_seq_l6.tiff", dst);
    t1 = now_ms();
    const ms_t seq_l6_read = t1 - t0;

    print_row("sequential (level=6)", w, h, seq_l6_write, seq_l6_read);

    // ---- Parallel tiled, level 1 (fastest) ----
    t0 = now_ms();
    tiff_io::write("_par_l1.tiff", src, nullptr, {.tile_size = 512, .compression_level = 1});
    t1 = now_ms();
    const ms_t par_l1_write = t1 - t0;

    t0 = now_ms();
    tiff_io::read("_par_l1.tiff", dst);
    t1 = now_ms();
    const ms_t par_l1_read = t1 - t0;

    print_row("parallel tile=512 level=1", w, h, par_l1_write, par_l1_read);

    // ---- Parallel tiled, level 6 (default) ----
    t0 = now_ms();
    tiff_io::write("_par_l6.tiff", src, nullptr, {.tile_size = 512, .compression_level = 6});
    t1 = now_ms();
    const ms_t par_l6_write = t1 - t0;

    t0 = now_ms();
    tiff_io::read("_par_l6.tiff", dst);
    t1 = now_ms();
    const ms_t par_l6_read = t1 - t0;

    print_row("parallel tile=512 level=6", w, h, par_l6_write, par_l6_read);

    // ---- Speedup summary (vs sequential no-compress) ----
    std::printf("  speedup vs seq-nocompress  write: x%.1f (par-l1)  x%.1f (par-l6)"
                "  |  read: x%.1f (par-l1)  x%.1f (par-l6)\n",
                seq_write / par_l1_write,
                seq_write / par_l6_write,
                seq_read  / par_l1_read,
                seq_read  / par_l6_read);
    std::printf("  speedup vs seq-l6          write: x%.1f (par-l1)  x%.1f (par-l6)"
                "  |  read: x%.1f (par-l1)  x%.1f (par-l6)\n",
                seq_l6_write / par_l1_write,
                seq_l6_write / par_l6_write,
                seq_l6_read  / par_l1_read,
                seq_l6_read  / par_l6_read);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    std::printf("hardware threads: %d\n", hw);

    bench(1024, 1024);
    bench(4096, 4096);

    // Cleanup temp files
    std::remove("_seq.tiff");
    std::remove("_seq_l1.tiff");
    std::remove("_seq_l6.tiff");
    std::remove("_par_l1.tiff");
    std::remove("_par_l6.tiff");
}

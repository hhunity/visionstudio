#include "io/tiff_io.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static image_data make_gradient(int w, int h)
{
    image_data img;
    img.width  = w;
    img.height = h;
    img.format = PixelFormat::rgb;
    img.pixels.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.pixels.data() + (y * w + x) * 3;
            p[0] = static_cast<uint8_t>(x & 0xFF);
            p[1] = static_cast<uint8_t>(y & 0xFF);
            p[2] = 128;
        }
    }
    return img;
}

static bool pixels_equal(const image_data& a, const image_data& b)
{
    if (a.width != b.width || a.height != b.height) return false;
    return std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()) == 0;
}

// Run one test case; returns true on pass.
static bool run_test(const std::string& name,
                     int w, int h,
                     const tiff_io::WriteOptions& opts)
{
    const std::string path = name + ".tiff";
    const image_data src = make_gradient(w, h);

    auto wopts = opts;
    wopts.output_format = PixelFormat::rgb;

    if (!tiff_io::write(path, src, nullptr, wopts)) {
        std::fprintf(stderr, "FAIL [%s] write error\n", name.c_str());
        return false;
    }

    image_data dst;
    if (!tiff_io::read(path, dst, nullptr, {.output_format = PixelFormat::rgb})) {
        std::fprintf(stderr, "FAIL [%s] read error\n", name.c_str());
        return false;
    }

    if (!pixels_equal(src, dst)) {
        std::fprintf(stderr, "FAIL [%s] pixel mismatch (%dx%d)\n",
                     name.c_str(), w, h);
        return false;
    }

    std::printf("PASS [%s] %dx%d tile=%u level=%d\n",
                name.c_str(), w, h, opts.tile_size, opts.compression_level);
    return true;
}

// Correctness test for single-strip (flat) write via write().
static bool run_test_flat(const std::string& name, int w, int h, int level = 0)
{
    const std::string path = name + ".tiff";
    const image_data src = make_gradient(w, h);

    const tiff_io::WriteOptions opts{
        .tile_size         = 0,
        .compression_level = level,
        .rows_per_strip    = static_cast<uint32_t>(h),
    };
    auto wopts = opts;
    wopts.output_format = PixelFormat::rgb;

    if (!tiff_io::write(path, src, nullptr, wopts)) {
        std::fprintf(stderr, "FAIL [%s] write error\n", name.c_str());
        return false;
    }

    image_data dst;
    if (!tiff_io::read(path, dst, nullptr, {.output_format = PixelFormat::rgb})) {
        std::fprintf(stderr, "FAIL [%s] read error\n", name.c_str());
        return false;
    }

    if (!pixels_equal(src, dst)) {
        std::fprintf(stderr, "FAIL [%s] pixel mismatch (%dx%d)\n",
                     name.c_str(), w, h);
        return false;
    }

    std::printf("PASS [%s] %dx%d (flat level=%d)\n", name.c_str(), w, h, level);
    return true;
}

// ---------------------------------------------------------------------------
// Write speed benchmark
// ---------------------------------------------------------------------------

static void benchmark(int w, int h, const tiff_io::WriteOptions& opts)
{
    const image_data img = make_gradient(w, h);

    auto t0 = std::chrono::high_resolution_clock::now();
    auto wopts = opts;
    wopts.output_format = PixelFormat::rgb;
    tiff_io::write("bench.tiff", img, nullptr, wopts);
    auto t1 = std::chrono::high_resolution_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double mb = static_cast<double>(w) * h * 4 / (1024.0 * 1024.0);
    std::printf("BENCH %dx%d (%.1f MB) tile=%u level=%d -> %.1f ms  (%.0f MB/s)\n",
                w, h, mb, opts.tile_size, opts.compression_level,
                ms, mb / (ms / 1000.0));
}

static void benchmark_flat(int w, int h, int level)
{
    benchmark(w, h, {
        .tile_size         = 0,
        .compression_level = level,
        .rows_per_strip    = static_cast<uint32_t>(h),
    });
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    int failed = 0;

    // --- Correctness tests ---

    // Tile-aligned sizes
    failed += !run_test("t_256x256",  256,  256, {});
    failed += !run_test("t_512x512",  512,  512, {});
    failed += !run_test("t_1024x768", 1024, 768, {});

    // Non-tile-aligned (edge tile padding)
    failed += !run_test("t_300x200",  300,  200, {});
    failed += !run_test("t_1x1",        1,    1, {});
    failed += !run_test("t_255x257",  255,  257, {});

    // Different tile sizes
    failed += !run_test("t_tile64",   512,  512, {.tile_size = 64});
    failed += !run_test("t_tile512",  512,  512, {.tile_size = 512});

    // Compression levels
    failed += !run_test("t_level1",   512,  512, {.compression_level = 1});
    failed += !run_test("t_level9",   512,  512, {.compression_level = 9});

    // Flat, no compression
    failed += !run_test_flat("t_flat_nc_512x512",  512,  512, 0);
    failed += !run_test_flat("t_flat_nc_300x200",  300,  200, 0);
    failed += !run_test_flat("t_flat_nc_1x1",        1,    1, 0);

    // Flat, with DEFLATE (single-block compression)
    failed += !run_test_flat("t_flat_l1_512x512",  512,  512, 1);
    failed += !run_test_flat("t_flat_l6_512x512",  512,  512, 6);
    failed += !run_test_flat("t_flat_l6_300x200",  300,  200, 6);

    std::printf("\n%s  (%d test(s) failed)\n",
                failed == 0 ? "ALL PASSED" : "SOME FAILED", failed);

    // --- Speed benchmark ---
    std::printf("\n--- benchmark ---\n");
    benchmark(4096, 4096, {.tile_size = 256, .compression_level = 1});
    benchmark(4096, 4096, {.tile_size = 256, .compression_level = 6});
    benchmark(4096, 4096, {.tile_size = 256, .compression_level = 9});
    benchmark(4096, 4096, {.tile_size = 512, .compression_level = 6});
    benchmark(4096, 4096, {.tile_size = 512, .compression_level = 6});
    benchmark(4096, 4096, {.tile_size = 512, .compression_level = 6 ,.output_format = PixelFormat::gray  });
    // benchmark_flat(4096, 4096, 0);  // no compression, baseline
    benchmark_flat(4096, 4096, 6);  // deflate level 6, single block

    return failed == 0 ? 0 : 1;
}

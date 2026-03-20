#include "io/tiff_io.h"
#include <tiffio.h>
#include <cstdio>
#include <vector>

namespace tiff_io {

// ---------------------------------------------------------------------------
// Fast-path helpers: read scanlines directly into out.pixels without an
// intermediate uint32_t raster.  Only called for known 8-bit formats.
// ---------------------------------------------------------------------------

static bool read_gray8(TIFF* tif, uint32_t w, uint32_t h, image_data& out) {
    std::vector<uint8_t> row(w);
    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(tif, row.data(), y, 0) < 0) return false;
        uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t v = row[x];
            dst[x * 4 + 0] = v;
            dst[x * 4 + 1] = v;
            dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }
    return true;
}

static bool read_rgb8(TIFF* tif, uint32_t w, uint32_t h, image_data& out) {
    std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(tif, row.data(), y, 0) < 0) return false;
        uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 3 + 0];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    return true;
}

static bool read_rgba8(TIFF* tif, uint32_t w, uint32_t h, image_data& out) {
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * row_bytes;
        if (TIFFReadScanline(tif, dst, y, 0) < 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fallback: generic conversion via TIFFReadRGBAImageOriented.
// Handles 1/2/4/16-bit, palette, CMYK, YCbCr, etc.
// ---------------------------------------------------------------------------

static bool read_generic(TIFF* tif, uint32_t w, uint32_t h, image_data& out) {
    std::vector<uint32_t> raster(static_cast<size_t>(w) * h);
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0))
        return false;

    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        out.pixels[i * 4 + 0] = TIFFGetR(raster[i]);
        out.pixels[i * 4 + 1] = TIFFGetG(raster[i]);
        out.pixels[i * 4 + 2] = TIFFGetB(raster[i]);
        out.pixels[i * 4 + 3] = TIFFGetA(raster[i]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool read(const std::string& path, image_data& out) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
        fprintf(stderr, "tiff_io::read: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

    if (w == 0 || h == 0) {
        TIFFClose(tif);
        return false;
    }

    // Detect format for fast-path selection.
    uint16_t photometric   = PHOTOMETRIC_RGB;
    uint16_t bits_per_sample  = 8;
    uint16_t samples_per_pixel = 1;
    uint16_t planar_config = PLANARCONFIG_CONTIG;
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,    &photometric);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE,  &bits_per_sample);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL,&samples_per_pixel);
    TIFFGetField(tif, TIFFTAG_PLANARCONFIG,   &planar_config);

    out.width  = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    const bool is_contig = (planar_config == PLANARCONFIG_CONTIG);
    const bool is_8bit   = (bits_per_sample == 8);

    bool ok = false;
    if (is_8bit && is_contig) {
        const bool is_gray = (photometric == PHOTOMETRIC_MINISBLACK ||
                              photometric == PHOTOMETRIC_MINISWHITE);
        if (is_gray && samples_per_pixel == 1) {
            ok = read_gray8(tif, w, h, out);
        } else if (photometric == PHOTOMETRIC_RGB && samples_per_pixel == 3) {
            ok = read_rgb8(tif, w, h, out);
        } else if (photometric == PHOTOMETRIC_RGB && samples_per_pixel == 4) {
            ok = read_rgba8(tif, w, h, out);
        }
    }

    // Fall back to generic path for unsupported or complex formats.
    if (!ok) {
        ok = read_generic(tif, w, h, out);
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
        // TIFFWriteScanline expects non-const tdata_t; the cast is safe (no write to buffer).
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

#include "io/tiff_io.h"
#include <tiffio.h>
#include <cstdio>

namespace tiff_io {

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

    std::vector<uint32_t> raster(static_cast<size_t>(w) * h);
    // ORIENTATION_TOPLEFT ensures row 0 is the top of the image.
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
        fprintf(stderr, "tiff_io::read: TIFFReadRGBAImageOriented failed for '%s'\n", path.c_str());
        TIFFClose(tif);
        return false;
    }
    TIFFClose(tif);

    out.width  = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    // TIFFReadRGBAImage stores R in the lowest byte, A in the highest byte.
    // Use the TIFFGetR/G/B/A macros for portability across endianness.
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        out.pixels[i * 4 + 0] = TIFFGetR(raster[i]);
        out.pixels[i * 4 + 1] = TIFFGetG(raster[i]);
        out.pixels[i * 4 + 2] = TIFFGetB(raster[i]);
        out.pixels[i * 4 + 3] = TIFFGetA(raster[i]);
    }

    return true;
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

#include "external/cpplib/io/tiff_io.h"
#include <atomic>
#include <cstdio>
#include <string>

// Usage: tiff_to_gray <input.tiff> [output.tiff]
// Converts any TIFF to 8-bit grayscale using ITU-R BT.601 luminance weights.
// If output path is omitted, writes to <input_stem>_gray.tiff.

static std::string default_output(const std::string& input) {
    const auto dot = input.rfind('.');
    const std::string stem = (dot != std::string::npos) ? input.substr(0, dot) : input;
    return stem + "_gray.tiff";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tiff_to_gray <input.tiff> [output.tiff]\n");
        return 1;
    }
    const std::string in_path  = argv[1];
    const std::string out_path = (argc >= 3) ? argv[2] : default_output(in_path);

    std::atomic<float> progress{0.0f};
    image_data img;
    fprintf(stderr, "Reading  : %s\n", in_path.c_str());
    if (!tiff_io::read(in_path, img, &progress, tiff_io::ReadOptions{.output_format = PixelFormat::gray})) {
        fprintf(stderr, "Error: failed to read '%s'\n", in_path.c_str());
        return 1;
    }
    fprintf(stderr, "Size     : %d x %d  (gray, %zu bytes)\n",
            img.width, img.height, img.pixels.size());

    fprintf(stderr, "Writing  : %s\n", out_path.c_str());
    progress.store(0.0f);
    if (!tiff_io::write(out_path, img, &progress,
                        tiff_io::WriteOptions{.output_format = PixelFormat::gray})) {
        fprintf(stderr, "Error: failed to write '%s'\n", out_path.c_str());
        return 1;
    }
    fprintf(stderr, "Done.\n");
    return 0;
}

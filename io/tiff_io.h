#pragma once
#include <atomic>
#include <string>
#include "util/image_data.h"

namespace tiff_io {

// Read a TIFF file into image_data (converted to RGBA internally).
// progress: optional atomic updated 0.0→1.0 as rows are decoded (for UI feedback).
// Returns true on success.
bool read(const std::string& path, image_data& out,
          std::atomic<float>* progress = nullptr);

// Write image_data (RGBA) to a TIFF file.
// Returns true on success.
bool write(const std::string& path, const image_data& img);

} // namespace tiff_io

#pragma once
#include <string>
#include "util/image_data.h"

namespace tiff_io {

// Read a TIFF file into image_data (converted to RGBA internally).
// Returns true on success.
bool read(const std::string& path, image_data& out);

// Write image_data (RGBA) to a TIFF file.
// Returns true on success.
bool write(const std::string& path, const image_data& img);

} // namespace tiff_io

#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "util/image_data.h"

// IO_API is empty when building as a static library.
#define IO_API

namespace tiff_io {

struct WriteOptions {
    uint32_t tile_size         = 512;
    int      compression_level = 6;
};

struct ReadOptions {
    int max_threads = 0;
};

IO_API bool read(const std::string& path, image_data& out,
                 std::atomic<float>* progress = nullptr,
                 const ReadOptions& opts = {});

IO_API bool write(const std::string& path, const image_data& img,
                  const WriteOptions& opts = {});

} // namespace tiff_io

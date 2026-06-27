#pragma once
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include "util/image_data.h"
#include "external/cpplib/io/tiff_io.h"

struct async_loader {
    std::future<image_data> future;
    std::atomic<float>      progress{0.0f};
    std::atomic<bool>       cancel{false};
    bool                    active = false;
    std::string             path;

    async_loader()                               = default;
    async_loader(const async_loader&)            = delete;
    async_loader& operator=(const async_loader&) = delete;

    ~async_loader() {
        cancel.store(true);
        if (future.valid()) future.wait();
    }

    void start(std::string p) {
        path = p;
        progress.store(0.0f);
        cancel.store(false);
        active = true;
        auto* prog = &progress;
        future = std::async(std::launch::async, [p = std::move(p), prog]() {
            image_data img;
            const PixelFormat native = tiff_io::detect_format(p);
            const PixelFormat fmt    = (native == PixelFormat::gray) ? PixelFormat::gray
                                                                      : PixelFormat::rgba;
            if (!tiff_io::read(p, img, prog, tiff_io::ReadOptions{.output_format = fmt}))
                img.pixels.clear();
            return img;
        });
    }

    bool poll(image_data& out) {
        if (!active) return false;
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        out    = future.get();
        active = false;
        return true;
    }
};

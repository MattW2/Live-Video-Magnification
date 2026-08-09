#include "export/ScratchFileCache.hpp"

#include <filesystem>
#include <system_error>

namespace livim {

ScratchFileCache::~ScratchFileCache() {
    cleanup();
}

bool ScratchFileCache::open(const std::string& path, double fps, cv::Size size) {
    cleanup();
    path_ = path;
    frameCount_ = 0;
    complete_ = false;

    // Motion-JPEG (.avi) for fast, zero-dependency temporary scratch caching
    const int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    writerOpen_ = writer_.open(path_, fourcc, fps, size, true);
    return writerOpen_;
}

void ScratchFileCache::writeFrame(const cv::Mat& frame) {
    if (!writerOpen_ || frame.empty()) return;
    cv::Mat bgr = frame;
    if (frame.channels() == 1) {
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    }
    writer_.write(bgr);
    frameCount_++;
}

void ScratchFileCache::finalize() {
    if (writerOpen_) {
        writer_.release();
        writerOpen_ = false;
        complete_ = true;
    }
}

void ScratchFileCache::cleanup() {
    if (writerOpen_) {
        writer_.release();
        writerOpen_ = false;
    }
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
    frameCount_ = 0;
    complete_ = false;
}

} // namespace livim

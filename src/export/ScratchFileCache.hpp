#pragma once

#include <cstdint>
#include <string>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace livim {

class ScratchFileCache {
public:
    ScratchFileCache() = default;
    ~ScratchFileCache();

    bool open(const std::string& path, double fps, cv::Size size);
    void writeFrame(const cv::Mat& frame);
    void finalize();
    void cleanup();

    bool isOpen() const { return writerOpen_; }
    bool isComplete() const { return complete_; }
    const std::string& path() const { return path_; }
    std::int64_t frameCount() const { return frameCount_; }

private:
    cv::VideoWriter writer_;
    std::string path_;
    std::int64_t frameCount_ = 0;
    bool writerOpen_ = false;
    bool complete_ = false;
};

} // namespace livim

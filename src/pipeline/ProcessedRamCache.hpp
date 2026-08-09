#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <opencv2/core.hpp>

#include "processing/IProcessor.hpp"

namespace livim {

class ProcessedRamCache {
public:
    ProcessedRamCache() = default;
    ~ProcessedRamCache() = default;

    void clear();

    void addFrame(const cv::Mat& processed, const cv::Mat& original);

    bool isComplete() const { return complete_; }
    void setComplete(bool complete) { complete_ = complete; }

    std::size_t frameCount() const { return processedFrames_.size(); }
    std::size_t memoryUsageBytes() const;

    std::int64_t inFrame() const { return inFrame_; }
    std::int64_t outFrame() const { return outFrame_; }
    double fps() const { return fps_; }

    void setRangeAndFps(std::int64_t inFrame, std::int64_t outFrame, double fps) {
        inFrame_ = inFrame;
        outFrame_ = outFrame;
        fps_ = fps;
    }

    const ProcessorConfig& config() const { return config_; }
    void setConfig(const ProcessorConfig& cfg) { config_ = cfg; }

    bool getFrame(std::int64_t frameIndex, cv::Mat& outProcessed, cv::Mat& outOriginal) const;

private:
    std::vector<cv::Mat> processedFrames_;
    std::vector<cv::Mat> originalFrames_;
    std::int64_t inFrame_ = 0;
    std::int64_t outFrame_ = 0;
    double fps_ = 30.0;
    ProcessorConfig config_;
    bool complete_ = false;
};

} // namespace livim

#include "pipeline/ProcessedRamCache.hpp"

namespace livim {

void ProcessedRamCache::clear() {
    processedFrames_.clear();
    originalFrames_.clear();
    inFrame_ = 0;
    outFrame_ = 0;
    complete_ = false;
}

void ProcessedRamCache::addFrame(const cv::Mat& processed, const cv::Mat& original) {
    processedFrames_.push_back(processed.clone());
    originalFrames_.push_back(original.clone());
}

std::size_t ProcessedRamCache::memoryUsageBytes() const {
    std::size_t total = 0;
    for (const auto& m : processedFrames_) {
        total += m.total() * m.elemSize();
    }
    for (const auto& m : originalFrames_) {
        total += m.total() * m.elemSize();
    }
    return total;
}

bool ProcessedRamCache::getFrame(std::int64_t frameIndex, cv::Mat& outProcessed, cv::Mat& outOriginal) const {
    if (!complete_ || frameIndex < inFrame_ || processedFrames_.empty()) return false;
    const std::size_t offset = static_cast<std::size_t>(frameIndex - inFrame_);
    if (offset >= processedFrames_.size()) return false;

    outProcessed = processedFrames_[offset];
    if (offset < originalFrames_.size()) {
        outOriginal = originalFrames_[offset];
    } else {
        outOriginal = outProcessed;
    }
    return true;
}

} // namespace livim

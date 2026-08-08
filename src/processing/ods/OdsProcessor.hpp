#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/OdsTypes.hpp"

namespace livim {

class OdsProcessor {
public:
    using ProgressCallback = std::function<bool(int currentFrame, int totalFrames)>; // return false to cancel

    // Executes offline optical flow displacement extraction across video timeline range [inFrame, outFrame)
    // for given ROIs. Populates roi.displacementSignal for each ROI and outputs detected video FPS.
    static bool extractDisplacements(const std::string& videoPath,
                                    double& outFps,
                                    std::vector<ROI>& rois,
                                    std::int64_t inFrame = 0,
                                    std::int64_t outFrame = -1,
                                    ProgressCallback progressCb = nullptr);
};

} // namespace livim

#include "processing/ods/OdsProcessor.hpp"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/videoio.hpp>

namespace livim {

bool OdsProcessor::extractDisplacements(const std::string& videoPath,
                                        double& outFps,
                                        std::vector<ROI>& rois,
                                        std::int64_t inFrame,
                                        std::int64_t outFrame,
                                        ProgressCallback progressCb) {
    if (rois.empty()) {
        return false;
    }

    cv::VideoCapture cap(videoPath);
    if (!cap.isOpened()) {
        return false;
    }

    outFps = cap.get(cv::CAP_PROP_FPS);
    if (outFps <= 0.0) outFps = 30.0;

    const int totalVideoFrames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    if (totalVideoFrames <= 0) {
        return false;
    }

    int startF = static_cast<int>(std::max<std::int64_t>(0, inFrame));
    int endF = (outFrame <= 0 || outFrame > totalVideoFrames) ? totalVideoFrames : static_cast<int>(outFrame);

    if (startF >= endF) {
        return false;
    }

    const int targetFrames = endF - startF;

    // Seek cap to startF
    cap.set(cv::CAP_PROP_POS_FRAMES, startF);

    // Clear previous signals
    for (ROI& roi : rois) {
        roi.displacementSignal.clear();
        roi.displacementSignal.reserve(static_cast<std::size_t>(targetFrames));
    }

    cv::Mat f0;
    if (!cap.read(f0) || f0.empty()) {
        return false;
    }

    cv::Mat f0Gray;
    if (f0.channels() > 1) {
        cv::cvtColor(f0, f0Gray, cv::COLOR_BGR2GRAY);
    } else {
        f0Gray = f0;
    }

    const int imgW = f0Gray.cols;
    const int imgH = f0Gray.rows;

    std::vector<cv::Rect> pixelRects(rois.size());
    std::vector<cv::Mat> refRoiGrays(rois.size());

    for (std::size_t i = 0; i < rois.size(); ++i) {
        const QRectF& nr = rois[i].normalizedRect;
        int rx = static_cast<int>(std::round(nr.x() * imgW));
        int ry = static_cast<int>(std::round(nr.y() * imgH));
        int rw = static_cast<int>(std::round(nr.width() * imgW));
        int rh = static_cast<int>(std::round(nr.height() * imgH));

        rx = std::clamp(rx, 0, imgW - 1);
        ry = std::clamp(ry, 0, imgH - 1);
        rw = std::clamp(rw, 1, imgW - rx);
        rh = std::clamp(rh, 1, imgH - ry);

        pixelRects[i] = cv::Rect(rx, ry, rw, rh);
        refRoiGrays[i] = f0Gray(pixelRects[i]).clone();

        // Initial frame displacement is 0.0
        rois[i].displacementSignal.push_back(0.0f);
    }

    if (progressCb && !progressCb(1, targetFrames)) {
        return false;
    }

    std::vector<cv::Mat> prevRoiGrays = refRoiGrays;
    std::vector<float> accumX(rois.size(), 0.0f);
    std::vector<float> accumY(rois.size(), 0.0f);

    cv::Mat ft;
    cv::Mat flow;

    int processedCount = 1;
    while (processedCount < targetFrames && cap.read(ft) && !ft.empty()) {
        processedCount++;
        cv::Mat ftGray;
        if (ft.channels() > 1) {
            cv::cvtColor(ft, ftGray, cv::COLOR_BGR2GRAY);
        } else {
            ftGray = ft;
        }

        for (std::size_t i = 0; i < rois.size(); ++i) {
            cv::Mat currRoiGray = ftGray(pixelRects[i]);
            cv::Mat prevRoiGray = prevRoiGrays[i];

            // Farneback Optical Flow calculation
            cv::calcOpticalFlowFarneback(prevRoiGray, currRoiGray, flow, 0.5, 3, 15, 3, 5, 1.2, 0);

            // Spatial average flow vector for ROI
            double stepDx = 0.0;
            double stepDy = 0.0;
            const int roiPixels = flow.rows * flow.cols;

            for (int r = 0; r < flow.rows; ++r) {
                const cv::Point2f* fPtr = flow.ptr<cv::Point2f>(r);
                for (int c = 0; c < flow.cols; ++c) {
                    stepDx += fPtr[c].x;
                    stepDy += fPtr[c].y;
                }
            }

            if (roiPixels > 0) {
                stepDx /= static_cast<double>(roiPixels);
                stepDy /= static_cast<double>(roiPixels);
            }

            accumX[i] += static_cast<float>(stepDx);
            accumY[i] += static_cast<float>(stepDy);

            float dispScalar = 0.0f;
            switch (rois[i].axis) {
            case MotionAxis::AXIS_X:
                dispScalar = accumX[i];
                break;
            case MotionAxis::AXIS_Y:
                dispScalar = accumY[i];
                break;
            case MotionAxis::MAGNITUDE:
                dispScalar = std::sqrt(accumX[i] * accumX[i] + accumY[i] * accumY[i]);
                break;
            }

            rois[i].displacementSignal.push_back(dispScalar);
            prevRoiGrays[i] = currRoiGray.clone();
        }

        if (progressCb && !progressCb(processedCount, targetFrames)) {
            return false;
        }
    }

    return true;
}

} // namespace livim

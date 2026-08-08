#pragma once

#include <string>
#include <vector>
#include <QColor>
#include <QRectF>

namespace livim {

enum class MotionAxis {
    AXIS_X,
    AXIS_Y,
    MAGNITUDE
};

enum class WindowType {
    Hann,
    FlatTop,
    BlackmanHarris,
    None
};

struct ROI {
    int id = 0;
    std::string name;
    QRectF normalizedRect; // [0, 1] normalized bounds relative to video dimensions
    QColor color = Qt::red;
    MotionAxis axis = MotionAxis::AXIS_X;
    bool visible = true;
    
    // Extracted sub-pixel displacement waveform (px vs frame t)
    std::vector<float> displacementSignal;
};

struct SpectrumPeak {
    double frequencyHz = 0.0;
    double magnitudePx = 0.0;
    double magnitudeDb = 0.0;
};

struct RoiSpectrum {
    int roiId = 0;
    std::string roiName;
    QColor color;
    std::vector<double> frequenciesHz;
    std::vector<double> magnitudesPx;
    std::vector<double> magnitudesDb;
    std::vector<SpectrumPeak> topPeaks; // Top 3 dominant peaks
};

inline std::string motionAxisToString(MotionAxis axis) {
    switch (axis) {
    case MotionAxis::AXIS_X: return "X";
    case MotionAxis::AXIS_Y: return "Y";
    case MotionAxis::MAGNITUDE: return "Mag";
    }
    return "X";
}

} // namespace livim

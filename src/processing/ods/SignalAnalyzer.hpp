#pragma once

#include <vector>
#include "core/OdsTypes.hpp"

namespace livim {

class SignalAnalyzer {
public:
    // Computes single-sided magnitude spectrum for an ROI displacement signal.
    // Parameters:
    // - signal: Raw sub-pixel displacement waveform (px vs frame)
    // - fps: Video capture frame rate in Hz
    // - windowType: Window function to apply (Hann, FlatTop, BlackmanHarris, None)
    // Returns RoiSpectrum with frequencies (Hz), linear magnitudes (px), dB magnitudes, and top 3 peaks.
    static RoiSpectrum computeSpectrum(const ROI& roi, double fps, WindowType windowType);

    // Detrends a 1D signal in-place by removing mean (DC offset) and linear slope.
    static void detrend(std::vector<double>& signal);

    // Generates a window sequence of length N.
    static std::vector<double> generateWindow(WindowType type, std::size_t N);

    // Detects top N dominant peaks in the spectrum.
    static std::vector<SpectrumPeak> detectPeaks(const std::vector<double>& frequenciesHz,
                                                const std::vector<double>& magnitudesPx,
                                                const std::vector<double>& magnitudesDb,
                                                int topN = 3);
};

} // namespace livim

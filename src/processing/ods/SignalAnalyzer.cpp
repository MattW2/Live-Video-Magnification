#include "processing/ods/SignalAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <opencv2/core.hpp>

namespace livim {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void SignalAnalyzer::detrend(std::vector<double>& signal) {
    const std::size_t n = signal.size();
    if (n < 2) return;

    // 1. Remove mean (DC offset)
    double sum = 0.0;
    for (double val : signal) {
        sum += val;
    }
    const double mean = sum / static_cast<double>(n);
    for (double& val : signal) {
        val -= mean;
    }

    // 2. Least squares linear detrending: fit y = a * i + b
    const double doubleN = static_cast<double>(n);
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;

    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        const double y = signal[i];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }

    const double denom = doubleN * sumXX - sumX * sumX;
    if (std::abs(denom) > 1e-12) {
        const double slope = (doubleN * sumXY - sumX * sumY) / denom;
        const double intercept = (sumY - slope * sumX) / doubleN;
        for (std::size_t i = 0; i < n; ++i) {
            signal[i] -= (slope * static_cast<double>(i) + intercept);
        }
    }
}

std::vector<double> SignalAnalyzer::generateWindow(WindowType type, std::size_t N) {
    std::vector<double> w(N, 1.0);
    if (N <= 1 || type == WindowType::None) {
        return w;
    }

    const double nMinus1 = static_cast<double>(N - 1);
    for (std::size_t i = 0; i < N; ++i) {
        const double n = static_cast<double>(i);
        switch (type) {
        case WindowType::Hann:
            w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / nMinus1);
            break;
        case WindowType::FlatTop:
            w[i] = 0.21557895 
                 - 0.41663158 * std::cos(2.0 * kPi * n / nMinus1)
                 + 0.277263158 * std::cos(4.0 * kPi * n / nMinus1)
                 - 0.083578947 * std::cos(6.0 * kPi * n / nMinus1)
                 + 0.006947368 * std::cos(8.0 * kPi * n / nMinus1);
            break;
        case WindowType::BlackmanHarris:
            w[i] = 0.35875
                 - 0.48829 * std::cos(2.0 * kPi * n / nMinus1)
                 + 0.14128 * std::cos(4.0 * kPi * n / nMinus1)
                 - 0.01168 * std::cos(6.0 * kPi * n / nMinus1);
            break;
        case WindowType::None:
            w[i] = 1.0;
            break;
        }
    }
    return w;
}

RoiSpectrum SignalAnalyzer::computeSpectrum(const ROI& roi, double fps, WindowType windowType) {
    RoiSpectrum spec;
    spec.roiId = roi.id;
    spec.roiName = roi.name;
    spec.color = roi.color;

    if (roi.displacementSignal.size() < 4 || fps <= 0.0) {
        return spec;
    }

    const std::size_t N = roi.displacementSignal.size();
    std::vector<double> signal(N);
    for (std::size_t i = 0; i < N; ++i) {
        signal[i] = static_cast<double>(roi.displacementSignal[i]);
    }

    // Preprocessing: DC removal and linear detrending
    detrend(signal);

    // Apply Windowing
    const std::vector<double> win = generateWindow(windowType, N);
    double coherentGain = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        signal[i] *= win[i];
        coherentGain += win[i];
    }
    if (coherentGain <= 1e-12) coherentGain = 1.0;

    // Run DFT via OpenCV
    cv::Mat realInput(static_cast<int>(N), 1, CV_64F, signal.data());
    cv::Mat dftOutput;
    cv::dft(realInput, dftOutput, cv::DFT_COMPLEX_OUTPUT);

    const std::size_t numBins = N / 2 + 1;
    spec.frequenciesHz.reserve(numBins);
    spec.magnitudesPx.reserve(numBins);
    spec.magnitudesDb.reserve(numBins);

    const double df = fps / static_cast<double>(N);

    for (std::size_t k = 0; k < numBins; ++k) {
        const double freq = static_cast<double>(k) * df;
        const double re = dftOutput.at<cv::Vec2d>(static_cast<int>(k))[0];
        const double im = dftOutput.at<cv::Vec2d>(static_cast<int>(k))[1];
        const double rawMag = std::sqrt(re * re + im * im);

        // Normalize single-sided spectrum amplitude: (2 / coherentGain) * |DFT| for AC, (1 / coherentGain) for DC
        const double normFactor = (k == 0 || k == (N / 2)) ? (1.0 / coherentGain) : (2.0 / coherentGain);
        const double magPx = rawMag * normFactor;
        const double magDb = 20.0 * std::log10(std::max(magPx, 1e-12));

        spec.frequenciesHz.push_back(freq);
        spec.magnitudesPx.push_back(magPx);
        spec.magnitudesDb.push_back(magDb);
    }

    spec.topPeaks = detectPeaks(spec.frequenciesHz, spec.magnitudesPx, spec.magnitudesDb, 3);
    return spec;
}

std::vector<SpectrumPeak> SignalAnalyzer::detectPeaks(const std::vector<double>& frequenciesHz,
                                                       const std::vector<double>& magnitudesPx,
                                                       const std::vector<double>& magnitudesDb,
                                                       int topN) {
    std::vector<SpectrumPeak> peaks;
    const std::size_t N = magnitudesPx.size();
    if (N < 3) return peaks;

    // Find max amplitude to set noise floor
    double maxMag = 0.0;
    for (double mag : magnitudesPx) {
        if (mag > maxMag) maxMag = mag;
    }
    if (maxMag <= 1e-9) return peaks;

    const double noiseFloor = maxMag * 0.05; // 5% noise floor threshold

    for (std::size_t i = 1; i < N - 1; ++i) {
        if (magnitudesPx[i] > magnitudesPx[i - 1] && magnitudesPx[i] > magnitudesPx[i + 1]) {
            if (magnitudesPx[i] >= noiseFloor) {
                SpectrumPeak peak;
                peak.frequencyHz = frequenciesHz[i];
                peak.magnitudePx = magnitudesPx[i];
                peak.magnitudeDb = magnitudesDb[i];
                peaks.push_back(peak);
            }
        }
    }

    // Sort peaks in descending order of magnitudePx
    std::sort(peaks.begin(), peaks.end(), [](const SpectrumPeak& a, const SpectrumPeak& b) {
        return a.magnitudePx > b.magnitudePx;
    });

    if (static_cast<int>(peaks.size()) > topN) {
        peaks.resize(static_cast<std::size_t>(topN));
    }

    return peaks;
}

} // namespace livim

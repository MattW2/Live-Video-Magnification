#pragma once

#include <cstdint>
#include <vector>
#include <QDialog>

#include "core/OdsTypes.hpp"

class QComboBox;
class QCheckBox;
class QLabel;
class QVBoxLayout;
class QHBoxLayout;
class QTabWidget;

class QChart;
class QChartView;
class QLineSeries;
class QValueAxis;

namespace livim {

class OdsAnalysisDialog : public QDialog {
    Q_OBJECT
public:
    explicit OdsAnalysisDialog(const std::vector<ROI>& rois,
                              std::int64_t startFrame,
                              std::int64_t endFrame,
                              double fps,
                              QWidget* parent = nullptr);
    ~OdsAnalysisDialog() override = default;

    void updateData(const std::vector<ROI>& rois, std::int64_t startFrame, std::int64_t endFrame, double fps);
    void setPlayheadFrame(std::int64_t frameIndex);

signals:
    void seekRequested(std::int64_t frameIndex);

private slots:
    void onWindowTypeChanged(int index);
    void onScaleToggled(bool useDb);
    void updateCharts();

private:
    void calculateSpectra();
    QWidget* createFreqTab();
    QWidget* createTimeTab();
    void updateFreqChart();
    void updateTimeChart();
    void updatePlayheadCursor();

    std::vector<ROI> rois_;
    std::int64_t startFrame_ = 0;
    std::int64_t endFrame_ = 0;
    double fps_ = 30.0;
    std::int64_t currentPlayheadFrame_ = 0;

    WindowType windowType_ = WindowType::Hann;
    bool useDbScale_ = false;

    std::vector<RoiSpectrum> spectra_;

    QTabWidget* tabWidget_ = nullptr;
    QComboBox* windowCombo_ = nullptr;
    QCheckBox* dbScaleCheck_ = nullptr;
    QVBoxLayout* roiCheckLayout_ = nullptr;
    std::vector<QCheckBox*> roiCheckBoxes_;
    QLabel* cursorInfoLabel_ = nullptr;

    // Frequency Spectrum Chart
    QChart* chartFreq_ = nullptr;
    QChartView* chartViewFreq_ = nullptr;
    QValueAxis* axisFreqX_ = nullptr;
    QValueAxis* axisFreqY_ = nullptr;

    // Time Waveform Chart
    QChart* chartTime_ = nullptr;
    QChartView* chartViewTime_ = nullptr;
    QValueAxis* axisTimeX_ = nullptr;
    QValueAxis* axisTimeY_ = nullptr;
    QLineSeries* playheadCursorSeries_ = nullptr;
};

} // namespace livim

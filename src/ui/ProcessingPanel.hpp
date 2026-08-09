#pragma once

#include <QWidget>

#include "processing/IProcessor.hpp" // MagnificationMode, MagnificationParams

#include "ui/DisplayWidget.hpp"

class QEvent;
class QGroupBox;
class QPushButton;
class QComboBox;

namespace livim {

class MagnificationControls;
class SegmentedControl;
class ToggleSwitch;
class RoiManagerWidget;

// Right-hand processing inspector. Source-agnostic: emits intent signals MainWindow forwards to
// PlaybackController, and exposes setters the window calls to reflect source state.
class ProcessingPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProcessingPanel(QWidget* parent = nullptr);

    // Disabled for an already single-channel source.
    void setGrayscaleAvailable(bool available);

    // Caps the Levels control to what the source resolution supports.
    void setMaxLevels(int maxLevels);

    // Clamps the Hz cutoffs to Nyquist (fps/2).
    void setCaptureFps(double fps);
    double captureFps() const;

    // Shows/hides the "Reset ROI" button.
    void setRoiActive(bool active);

    // Reflects the "Select ROI" toggle state without re-emitting.
    void setRoiSelecting(bool selecting);

    RoiManagerWidget* roiManager() const { return roiManager_; }

    // Direct passband frequency tuning from spectrum analysis
    void setFrequencyBand(double fLow, double fHigh);

signals:
    void grayscaleToggled(bool enabled);
    void magnificationChanged(MagnificationParams params);
    void downscaleChanged(int divisor); // 1 / 2 / 4 / 8
    void roiSelectModeChanged(bool selecting);
    void roiResetRequested();
    void opticalFlowOverlayChanged(DisplayWidget::OpticalFlowOverlayMode mode);

protected:
    void changeEvent(QEvent* event) override;

private:
    void refreshIcons();

    ToggleSwitch*     grayscaleSwitch_ = nullptr;
    SegmentedControl* resolutionSeg_ = nullptr;
    QComboBox*        flowOverlayCombo_ = nullptr;
    QPushButton*      roiSelectButton_ = nullptr;
    QPushButton*      roiResetButton_ = nullptr;

    QGroupBox*             magGroup_ = nullptr;
    MagnificationControls* magControls_ = nullptr;

    QGroupBox*        odsGroup_ = nullptr;
    RoiManagerWidget* roiManager_ = nullptr;
};

} // namespace livim

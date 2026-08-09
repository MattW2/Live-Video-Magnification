#include "ui/OdsAnalysisDialog.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <QCheckBox>
#include <QComboBox>
#include <QGraphicsSimpleTextItem>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include "processing/ods/SignalAnalyzer.hpp"

namespace livim {

namespace {

class InteractiveChartView : public QChartView {
public:
    using QChartView::QChartView;
    std::function<void(double timeSeconds)> onTimeClicked;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && chart()) {
            const QPointF val = chart()->mapToValue(event->position());
            if (onTimeClicked) onTimeClicked(val.x());
        }
        QChartView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton && chart()) {
            const QPointF val = chart()->mapToValue(event->position());
            if (onTimeClicked) onTimeClicked(val.x());
        }
        QChartView::mouseMoveEvent(event);
    }
};

} // namespace

OdsAnalysisDialog::OdsAnalysisDialog(const std::vector<ROI>& rois,
                                     std::int64_t startFrame,
                                     std::int64_t endFrame,
                                     double fps,
                                     QWidget* parent)
    : QDialog(parent), rois_(rois), startFrame_(startFrame), endFrame_(endFrame), fps_(fps) {
    setWindowTitle(tr("Video ODS Multi-ROI Analysis System"));
    resize(1080, 720);
    setStyleSheet("QDialog { background-color: #121212; color: white; }");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    // Top Controls Bar
    auto* controlsBox = new QGroupBox(tr("Analysis & Display Controls"), this);
    controlsBox->setStyleSheet("QGroupBox { background-color: #1e1e1e; border: 1px solid #333333; border-radius: 6px; color: white; font-weight: bold; margin-top: 6px; padding-top: 12px; }");
    auto* controlsLayout = new QHBoxLayout(controlsBox);

    // Windowing Selector
    controlsLayout->addWidget(new QLabel(tr("Window:"), this));
    windowCombo_ = new QComboBox(this);
    windowCombo_->addItem("Hann", static_cast<int>(WindowType::Hann));
    windowCombo_->addItem("Flat Top", static_cast<int>(WindowType::FlatTop));
    windowCombo_->addItem("Blackman-Harris", static_cast<int>(WindowType::BlackmanHarris));
    windowCombo_->addItem("None (Rectangular)", static_cast<int>(WindowType::None));
    windowCombo_->setStyleSheet("QComboBox { background-color: #2d2d2d; color: white; border-radius: 4px; padding: 4px 8px; border: 1px solid #444; }");
    connect(windowCombo_, &QComboBox::currentIndexChanged, this, &OdsAnalysisDialog::onWindowTypeChanged);
    controlsLayout->addWidget(windowCombo_);

    controlsLayout->addSpacing(16);

    // Scale Toggle
    dbScaleCheck_ = new QCheckBox(tr("Logarithmic Scale (dB)"), this);
    dbScaleCheck_->setStyleSheet("QCheckBox { color: white; font-weight: normal; }");
    connect(dbScaleCheck_, &QCheckBox::toggled, this, &OdsAnalysisDialog::onScaleToggled);
    controlsLayout->addWidget(dbScaleCheck_);

    controlsLayout->addSpacing(16);

    // Cursor / Hover Info Readout Label
    cursorInfoLabel_ = new QLabel(tr("Hover chart to inspect parameters"), this);
    cursorInfoLabel_->setStyleSheet("QLabel { color: #38bdf8; font-weight: bold; padding: 4px 8px; background-color: #2d2d2d; border-radius: 4px; border: 1px solid #444; }");
    controlsLayout->addWidget(cursorInfoLabel_);

    controlsLayout->addSpacing(16);

    auto* tuneBtn = new QPushButton(tr("Tune Magnification to Peak"), this);
    tuneBtn->setToolTip(tr("Automatically set the video magnification passband to isolate the dominant peak frequency"));
    tuneBtn->setStyleSheet(
        "QPushButton { background-color: #0284c7; color: white; border-radius: 4px; padding: 4px 10px; font-weight: bold; border: none; }"
        "QPushButton:hover { background-color: #0369a1; }"
    );
    connect(tuneBtn, &QPushButton::clicked, this, [this] {
        double bestFreq = -1.0;
        double maxAmp = -1.0;
        for (const RoiSpectrum& spec : spectra_) {
            for (const SpectrumPeak& p : spec.topPeaks) {
                if (p.magnitudePx > maxAmp) {
                    maxAmp = p.magnitudePx;
                    bestFreq = p.frequencyHz;
                }
            }
        }
        if (bestFreq > 0.0) {
            double fLow = std::max(0.05, bestFreq * 0.85);
            double fHigh = bestFreq * 1.15;
            emit bandpassTuned(fLow, fHigh);
        }
    });
    controlsLayout->addWidget(tuneBtn);

    controlsLayout->addStretch();
    mainLayout->addWidget(controlsBox);

    // Main Middle Content: Tab Widget + ROI Legend Sidebar
    auto* contentLayout = new QHBoxLayout();

    tabWidget_ = new QTabWidget(this);
    tabWidget_->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #333333; background-color: #1e1e1e; border-radius: 6px; }"
        "QTabBar::tab { background-color: #2d2d2d; color: white; padding: 8px 16px; border-top-left-radius: 4px; border-top-right-radius: 4px; font-weight: bold; }"
        "QTabBar::tab:selected { background-color: #0284c7; color: white; }"
    );

    tabWidget_->addTab(createFreqTab(), tr("Frequency Spectrum (Hz Only)"));
    tabWidget_->addTab(createTimeTab(), tr("Displacement Time Waveform (px vs Time)"));

    contentLayout->addWidget(tabWidget_, 4);

    // ROI Legend Sidebar Panel
    auto* legendBox = new QGroupBox(tr("Active ROIs"), this);
    legendBox->setStyleSheet("QGroupBox { background-color: #1e1e1e; border: 1px solid #333333; border-radius: 6px; color: white; font-weight: bold; margin-top: 6px; padding-top: 12px; }");
    roiCheckLayout_ = new QVBoxLayout(legendBox);

    for (std::size_t i = 0; i < rois_.size(); ++i) {
        const ROI& roi = rois_[i];
        auto* check = new QCheckBox(QString::fromStdString(roi.name + " [" + motionAxisToString(roi.axis) + "]"), this);
        check->setChecked(roi.visible);
        check->setStyleSheet(QString("QCheckBox { color: %1; font-weight: bold; }").arg(roi.color.name()));
        connect(check, &QCheckBox::toggled, this, &OdsAnalysisDialog::updateCharts);
        roiCheckLayout_->addWidget(check);
        roiCheckBoxes_.push_back(check);
    }
    roiCheckLayout_->addStretch();
    contentLayout->addWidget(legendBox, 1);

    mainLayout->addLayout(contentLayout, 1);

    // Bottom Action Buttons
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setStyleSheet("QPushButton { background-color: #2d2d2d; color: white; border-radius: 4px; padding: 6px 16px; font-weight: bold; border: 1px solid #444; }"
                             "QPushButton:hover { background-color: #383838; }");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(closeBtn);
    mainLayout->addLayout(bottomLayout);

    calculateSpectra();
    updateCharts();
}

void OdsAnalysisDialog::updateData(const std::vector<ROI>& rois, std::int64_t startFrame, std::int64_t endFrame, double fps) {
    rois_ = rois;
    startFrame_ = startFrame;
    endFrame_ = endFrame;
    fps_ = fps;

    // Update check boxes
    for (QCheckBox* cb : roiCheckBoxes_) {
        delete cb;
    }
    roiCheckBoxes_.clear();

    for (const ROI& roi : rois_) {
        auto* check = new QCheckBox(QString::fromStdString(roi.name + " [" + motionAxisToString(roi.axis) + "]"), this);
        check->setChecked(roi.visible);
        check->setStyleSheet(QString("QCheckBox { color: %1; font-weight: bold; }").arg(roi.color.name()));
        connect(check, &QCheckBox::toggled, this, &OdsAnalysisDialog::updateCharts);
        roiCheckLayout_->addWidget(check);
        roiCheckBoxes_.push_back(check);
    }

    calculateSpectra();
    updateCharts();
}

QWidget* OdsAnalysisDialog::createFreqTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);

    chartFreq_ = new QChart();
    chartFreq_->setTitle(tr("Multi-ROI Displacement Frequency Spectrum (Hz Only)"));
    chartFreq_->setTheme(QChart::ChartThemeDark);
    chartFreq_->setBackgroundBrush(QBrush(QColor("#1e1e1e")));
    chartFreq_->setTitleBrush(QBrush(Qt::white));

    axisFreqX_ = new QValueAxis();
    axisFreqX_->setTitleText(tr("Frequency (Hz)"));
    axisFreqX_->setLabelFormat("%.1f");
    axisFreqX_->setTitleBrush(QBrush(Qt::white));
    axisFreqX_->setLabelsBrush(QBrush(Qt::white));

    axisFreqY_ = new QValueAxis();
    axisFreqY_->setTitleText(tr("Displacement Spectral Magnitude (px)"));
    axisFreqY_->setTitleBrush(QBrush(Qt::white));
    axisFreqY_->setLabelsBrush(QBrush(Qt::white));

    chartFreq_->addAxis(axisFreqX_, Qt::AlignBottom);
    chartFreq_->addAxis(axisFreqY_, Qt::AlignLeft);

    chartViewFreq_ = new QChartView(chartFreq_, page);
    chartViewFreq_->setRenderHint(QPainter::Antialiasing);
    chartViewFreq_->setStyleSheet("QChartView { border: none; }");
    layout->addWidget(chartViewFreq_);

    return page;
}

QWidget* OdsAnalysisDialog::createTimeTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);

    chartTime_ = new QChart();
    chartTime_->setTitle(tr("Sub-Pixel Displacement Time Waveform (px vs Time)"));
    chartTime_->setTheme(QChart::ChartThemeDark);
    chartTime_->setBackgroundBrush(QBrush(QColor("#1e1e1e")));
    chartTime_->setTitleBrush(QBrush(Qt::white));

    axisTimeX_ = new QValueAxis();
    axisTimeX_->setTitleText(tr("Time (seconds)"));
    axisTimeX_->setLabelFormat("%.2f s");
    axisTimeX_->setTitleBrush(QBrush(Qt::white));
    axisTimeX_->setLabelsBrush(QBrush(Qt::white));

    axisTimeY_ = new QValueAxis();
    axisTimeY_->setTitleText(tr("Displacement (px)"));
    axisTimeY_->setTitleBrush(QBrush(Qt::white));
    axisTimeY_->setLabelsBrush(QBrush(Qt::white));

    chartTime_->addAxis(axisTimeX_, Qt::AlignBottom);
    chartTime_->addAxis(axisTimeY_, Qt::AlignLeft);

    auto* interactiveView = new InteractiveChartView(chartTime_, page);
    interactiveView->setRenderHint(QPainter::Antialiasing);
    interactiveView->setStyleSheet("QChartView { border: none; }");

    interactiveView->onTimeClicked = [this](double timeSeconds) {
        if (fps_ <= 0.0) return;
        std::int64_t frameOffset = static_cast<std::int64_t>(std::round(timeSeconds * fps_));
        std::int64_t targetFrame = startFrame_ + frameOffset;

        const std::int64_t maxFrame = std::max<std::int64_t>(startFrame_, endFrame_ > 0 ? endFrame_ - 1 : startFrame_);
        targetFrame = std::clamp(targetFrame, startFrame_, maxFrame);

        setPlayheadFrame(targetFrame);
        emit seekRequested(targetFrame);
    };

    chartViewTime_ = interactiveView;
    layout->addWidget(chartViewTime_);

    return page;
}

void OdsAnalysisDialog::onWindowTypeChanged(int index) {
    windowType_ = static_cast<WindowType>(windowCombo_->itemData(index).toInt());
    calculateSpectra();
    updateCharts();
}

void OdsAnalysisDialog::onScaleToggled(bool useDb) {
    useDbScale_ = useDb;
    axisFreqY_->setTitleText(useDbScale_ ? tr("Displacement Spectral Magnitude (dB)") : tr("Displacement Spectral Magnitude (px)"));
    updateFreqChart();
}

void OdsAnalysisDialog::calculateSpectra() {
    spectra_.clear();
    spectra_.reserve(rois_.size());
    for (const ROI& roi : rois_) {
        spectra_.push_back(SignalAnalyzer::computeSpectrum(roi, fps_, windowType_));
    }
}

void OdsAnalysisDialog::updateCharts() {
    updateFreqChart();
    updateTimeChart();
}

void OdsAnalysisDialog::updateFreqChart() {
    chartFreq_->removeAllSeries();

    double maxHz = fps_ * 0.5; // Nyquist limit
    double maxY = 1e-6;
    double minY = 0.0;

    if (useDbScale_) {
        minY = -100.0;
        maxY = 0.0;
    }

    for (std::size_t i = 0; i < spectra_.size(); ++i) {
        if (i < roiCheckBoxes_.size() && !roiCheckBoxes_[i]->isChecked()) {
            continue;
        }

        const RoiSpectrum& spec = spectra_[i];
        if (spec.frequenciesHz.empty()) continue;

        auto* series = new QLineSeries();
        series->setName(QString::fromStdString(spec.roiName));
        QPen pen(spec.color);
        pen.setWidth(2);
        series->setPen(pen);

        const std::vector<double>& yValues = useDbScale_ ? spec.magnitudesDb : spec.magnitudesPx;

        for (std::size_t k = 0; k < spec.frequenciesHz.size(); ++k) {
            const double hz = spec.frequenciesHz[k];
            const double yVal = yValues[k];
            series->append(hz, yVal);

            if (yVal > maxY) maxY = yVal;
            if (useDbScale_ && yVal < minY && yVal > -120.0) minY = yVal;
        }

        chartFreq_->addSeries(series);
        series->attachAxis(axisFreqX_);
        series->attachAxis(axisFreqY_);

        // Top 3 Peak Indicators
        if (!spec.topPeaks.empty()) {
            auto* peakSeries = new QScatterSeries();
            peakSeries->setName(QString::fromStdString(spec.roiName + " Peaks"));
            peakSeries->setMarkerShape(QScatterSeries::MarkerShapeCircle);
            peakSeries->setMarkerSize(8.0);
            peakSeries->setColor(spec.color);
            peakSeries->setBorderColor(Qt::white);

            for (const SpectrumPeak& peak : spec.topPeaks) {
                const double yPeak = useDbScale_ ? peak.magnitudeDb : peak.magnitudePx;
                peakSeries->append(peak.frequencyHz, yPeak);
            }

            connect(peakSeries, &QScatterSeries::clicked, this, [this](const QPointF& point) {
                const double fPeak = point.x();
                if (fPeak > 0.0) {
                    const double fLow = std::max(0.05, fPeak * 0.85);
                    const double fHigh = fPeak * 1.15;
                    emit bandpassTuned(fLow, fHigh);
                }
            });

            chartFreq_->addSeries(peakSeries);
            peakSeries->attachAxis(axisFreqX_);
            peakSeries->attachAxis(axisFreqY_);
        }

        connect(series, &QLineSeries::hovered, this, [this, spec](const QPointF& point, bool state) {
            if (state) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2);
                ss << spec.roiName << " -> Frequency: " << point.x() << " Hz, Amp: " << point.y() << (useDbScale_ ? " dB" : " px");
                cursorInfoLabel_->setText(QString::fromStdString(ss.str()));
            }
        });
    }

    axisFreqX_->setRange(0.0, std::max(1.0, maxHz));
    if (useDbScale_) {
        axisFreqY_->setRange(minY, maxY + 10.0);
    } else {
        axisFreqY_->setRange(0.0, maxY * 1.15 + 1e-4);
    }
}

void OdsAnalysisDialog::updateTimeChart() {
    chartTime_->removeAllSeries();
    playheadCursorSeries_ = nullptr;

    double maxTime = 0.0;
    double minY = 0.0;
    double maxY = 1e-6;

    for (std::size_t i = 0; i < rois_.size(); ++i) {
        if (i < roiCheckBoxes_.size() && !roiCheckBoxes_[i]->isChecked()) {
            continue;
        }

        const ROI& roi = rois_[i];
        if (roi.displacementSignal.empty()) continue;

        auto* series = new QLineSeries();
        series->setName(QString::fromStdString(roi.name + " [" + motionAxisToString(roi.axis) + "]"));
        QPen pen(roi.color);
        pen.setWidth(2);
        series->setPen(pen);

        const std::size_t n = roi.displacementSignal.size();
        const double dt = 1.0 / (fps_ > 0.0 ? fps_ : 30.0);

        for (std::size_t k = 0; k < n; ++k) {
            const double tSec = static_cast<double>(k) * dt;
            const double val = static_cast<double>(roi.displacementSignal[k]);
            series->append(tSec, val);

            if (tSec > maxTime) maxTime = tSec;
            if (val > maxY) maxY = val;
            if (val < minY) minY = val;
        }

        chartTime_->addSeries(series);
        series->attachAxis(axisTimeX_);
        series->attachAxis(axisTimeY_);

        connect(series, &QLineSeries::hovered, this, [this, roi, dt](const QPointF& point, bool state) {
            if (state) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(3);
                const int frameIdx = static_cast<int>(std::round(point.x() / dt)) + static_cast<int>(startFrame_);
                ss << roi.name << " [" << motionAxisToString(roi.axis) << "] -> Time: " << point.x() << " s (Frame " << frameIdx << "), Disp: " << point.y() << " px";
                cursorInfoLabel_->setText(QString::fromStdString(ss.str()));
            }
        });
    }

    // Add vertical Playhead Cursor Series
    playheadCursorSeries_ = new QLineSeries();
    playheadCursorSeries_->setName(tr("Playhead Cursor"));
    QPen cursorPen(QColor("#38bdf8"), 2, Qt::DashLine);
    playheadCursorSeries_->setPen(cursorPen);

    chartTime_->addSeries(playheadCursorSeries_);
    playheadCursorSeries_->attachAxis(axisTimeX_);
    playheadCursorSeries_->attachAxis(axisTimeY_);

    axisTimeX_->setRange(0.0, std::max(0.1, maxTime));

    const double yMargin = std::max(0.5, (maxY - minY) * 0.15);
    axisTimeY_->setRange(minY - yMargin, maxY + yMargin);

    updatePlayheadCursor();
}

void OdsAnalysisDialog::setPlayheadFrame(std::int64_t frameIndex) {
    currentPlayheadFrame_ = frameIndex;
    updatePlayheadCursor();
}

void OdsAnalysisDialog::updatePlayheadCursor() {
    if (!playheadCursorSeries_ || !axisTimeY_) return;

    playheadCursorSeries_->clear();

    const double dt = 1.0 / (fps_ > 0.0 ? fps_ : 30.0);
    const double relFrame = static_cast<double>(currentPlayheadFrame_ - startFrame_);
    const double currentTimeSec = relFrame * dt;

    const double minY = axisTimeY_->min();
    const double maxY = axisTimeY_->max();

    playheadCursorSeries_->append(currentTimeSec, minY);
    playheadCursorSeries_->append(currentTimeSec, maxY);
}

} // namespace livim

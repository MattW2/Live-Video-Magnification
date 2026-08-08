#pragma once

#include <vector>
#include <QWidget>

#include "core/OdsTypes.hpp"

class QTableWidget;
class QPushButton;

namespace livim {

class RoiManagerWidget : public QWidget {
    Q_OBJECT
public:
    explicit RoiManagerWidget(QWidget* parent = nullptr);
    ~RoiManagerWidget() override = default;

    const std::vector<ROI>& rois() const { return rois_; }
    void addRoi(const QRectF& normalizedRect);
    void setRois(const std::vector<ROI>& rois);
    void clearRois();

    void setDrawRoiArmed(bool armed);

signals:
    void roisChanged(const std::vector<ROI>& rois);
    void drawRoiToggled(bool armed);
    void runAnalysisRequested();

private:
    void rebuildTable();
    QColor generateColor(int index) const;

    std::vector<ROI> rois_;
    int nextId_ = 1;

    QTableWidget* tableWidget_ = nullptr;
    QPushButton* drawRoiBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* runAnalysisBtn_ = nullptr;
};

} // namespace livim

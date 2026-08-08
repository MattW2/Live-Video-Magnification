#include "ui/RoiManagerWidget.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace livim {

namespace {
const QColor kDefaultPalette[] = {
    QColor(239, 68, 68),   // Red
    QColor(59, 130, 246),  // Blue
    QColor(16, 185, 129),  // Green
    QColor(245, 158, 11),  // Amber / Yellow
    QColor(168, 85, 247),  // Purple
    QColor(236, 72, 153),  // Pink
    QColor(6, 182, 212),   // Cyan
    QColor(249, 115, 22),  // Orange
};
}

RoiManagerWidget::RoiManagerWidget(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(6);

    // Top Controls Bar
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);

    drawRoiBtn_ = new QPushButton(tr("Add ROI"), this);
    drawRoiBtn_->setCheckable(true);
    drawRoiBtn_->setStyleSheet(
        "QPushButton { background-color: #333333; color: white; border-radius: 4px; padding: 5px 10px; font-weight: bold; border: 1px solid #444; }"
        "QPushButton:checked { background-color: #0284c7; color: white; border-color: #38bdf8; }"
    );
    connect(drawRoiBtn_, &QPushButton::toggled, this, &RoiManagerWidget::drawRoiToggled);
    btnLayout->addWidget(drawRoiBtn_);

    clearBtn_ = new QPushButton(tr("Clear All"), this);
    clearBtn_->setStyleSheet(
        "QPushButton { background-color: #333333; color: white; border-radius: 4px; padding: 5px 10px; border: 1px solid #444; }"
        "QPushButton:hover { background-color: #444444; }"
    );
    connect(clearBtn_, &QPushButton::clicked, this, &RoiManagerWidget::clearRois);
    btnLayout->addWidget(clearBtn_);

    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // ROI Table
    tableWidget_ = new QTableWidget(this);
    tableWidget_->setColumnCount(5);
    tableWidget_->setHorizontalHeaderLabels({tr("Color"), tr("Name"), tr("Axis"), tr("Vis"), tr("Del")});
    tableWidget_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    tableWidget_->verticalHeader()->hide();
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setStyleSheet(
        "QTableWidget { background-color: #1e1e1e; color: white; gridline-color: #333333; border: 1px solid #333333; border-radius: 4px; }"
        "QHeaderView::section { background-color: #2d2d2d; color: white; padding: 4px; border: none; font-weight: bold; }"
    );
    mainLayout->addWidget(tableWidget_);

    // Run ODS Analysis Action Button
    runAnalysisBtn_ = new QPushButton(tr("Run Video ODS Analysis"), this);
    runAnalysisBtn_->setStyleSheet(
        "QPushButton { background-color: #0284c7; color: white; font-weight: bold; border-radius: 4px; padding: 8px 14px; font-size: 12px; border: none; }"
        "QPushButton:hover { background-color: #0369a1; }"
    );
    connect(runAnalysisBtn_, &QPushButton::clicked, this, &RoiManagerWidget::runAnalysisRequested);
    mainLayout->addWidget(runAnalysisBtn_);

    rebuildTable();
}

QColor RoiManagerWidget::generateColor(int index) const {
    const std::size_t count = sizeof(kDefaultPalette) / sizeof(kDefaultPalette[0]);
    return kDefaultPalette[index % count];
}

void RoiManagerWidget::setDrawRoiArmed(bool armed) {
    if (drawRoiBtn_->isChecked() != armed) {
        drawRoiBtn_->blockSignals(true);
        drawRoiBtn_->setChecked(armed);
        drawRoiBtn_->blockSignals(false);
    }
}

void RoiManagerWidget::addRoi(const QRectF& normalizedRect) {
    ROI roi;
    roi.id = nextId_++;
    roi.name = "ROI " + std::to_string(roi.id);
    roi.normalizedRect = normalizedRect;
    roi.color = generateColor(static_cast<int>(rois_.size()));
    roi.axis = MotionAxis::AXIS_X;
    roi.visible = true;

    rois_.push_back(roi);
    rebuildTable();
    emit roisChanged(rois_);
}

void RoiManagerWidget::setRois(const std::vector<ROI>& rois) {
    rois_ = rois;
    rebuildTable();
    emit roisChanged(rois_);
}

void RoiManagerWidget::clearRois() {
    rois_.clear();
    rebuildTable();
    emit roisChanged(rois_);
}

void RoiManagerWidget::rebuildTable() {
    tableWidget_->setRowCount(0);
    tableWidget_->setRowCount(static_cast<int>(rois_.size()));

    for (int r = 0; r < static_cast<int>(rois_.size()); ++r) {
        ROI& roi = rois_[r];

        // 0. Color Button
        auto* colorBtn = new QPushButton(tableWidget_);
        colorBtn->setFixedSize(22, 22);
        colorBtn->setStyleSheet(QString("background-color: %1; border: 1px solid white; border-radius: 3px;").arg(roi.color.name()));
        connect(colorBtn, &QPushButton::clicked, this, [this, r] {
            QColor picked = QColorDialog::getColor(rois_[r].color, this, tr("Select ROI Color"));
            if (picked.isValid()) {
                rois_[r].color = picked;
                rebuildTable();
                emit roisChanged(rois_);
            }
        });
        auto* colorWrapper = new QWidget(tableWidget_);
        auto* colorLayout = new QHBoxLayout(colorWrapper);
        colorLayout->setContentsMargins(2, 2, 2, 2);
        colorLayout->setAlignment(Qt::AlignCenter);
        colorLayout->addWidget(colorBtn);
        tableWidget_->setCellWidget(r, 0, colorWrapper);

        // 1. Name Editor
        auto* nameEdit = new QLineEdit(QString::fromStdString(roi.name), tableWidget_);
        nameEdit->setStyleSheet("QLineEdit { background: transparent; color: white; border: none; padding: 2px; }");
        connect(nameEdit, &QLineEdit::editingFinished, this, [this, r, nameEdit] {
            rois_[r].name = nameEdit->text().toStdString();
            emit roisChanged(rois_);
        });
        tableWidget_->setCellWidget(r, 1, nameEdit);

        // 2. Axis Dropdown
        auto* axisCombo = new QComboBox(tableWidget_);
        axisCombo->addItem("X", static_cast<int>(MotionAxis::AXIS_X));
        axisCombo->addItem("Y", static_cast<int>(MotionAxis::AXIS_Y));
        axisCombo->addItem("Mag", static_cast<int>(MotionAxis::MAGNITUDE));
        axisCombo->setCurrentIndex(static_cast<int>(roi.axis));
        axisCombo->setStyleSheet("QComboBox { background-color: #2d2d2d; color: white; border-radius: 3px; padding: 2px 4px; border: 1px solid #444; }");
        connect(axisCombo, &QComboBox::currentIndexChanged, this, [this, r, axisCombo](int index) {
            rois_[r].axis = static_cast<MotionAxis>(axisCombo->itemData(index).toInt());
            emit roisChanged(rois_);
        });
        tableWidget_->setCellWidget(r, 2, axisCombo);

        // 3. Visibility Checkbox
        auto* visCheck = new QCheckBox(tableWidget_);
        visCheck->setChecked(roi.visible);
        connect(visCheck, &QCheckBox::toggled, this, [this, r](bool checked) {
            rois_[r].visible = checked;
            emit roisChanged(rois_);
        });
        auto* visWrapper = new QWidget(tableWidget_);
        auto* visLayout = new QHBoxLayout(visWrapper);
        visLayout->setContentsMargins(2, 2, 2, 2);
        visLayout->setAlignment(Qt::AlignCenter);
        visLayout->addWidget(visCheck);
        tableWidget_->setCellWidget(r, 3, visWrapper);

        // 4. Delete Button
        auto* delBtn = new QPushButton("X", tableWidget_);
        delBtn->setFixedSize(20, 20);
        delBtn->setStyleSheet("QPushButton { background-color: rgba(239, 68, 68, 180); color: white; font-weight: bold; border-radius: 3px; border: none; }"
                               "QPushButton:hover { background-color: rgba(239, 68, 68, 255); }");
        connect(delBtn, &QPushButton::clicked, this, [this, r] {
            rois_.erase(rois_.begin() + r);
            rebuildTable();
            emit roisChanged(rois_);
        });
        auto* delWrapper = new QWidget(tableWidget_);
        auto* delLayout = new QHBoxLayout(delWrapper);
        delLayout->setContentsMargins(2, 2, 2, 2);
        delLayout->setAlignment(Qt::AlignCenter);
        delLayout->addWidget(delBtn);
        tableWidget_->setCellWidget(r, 4, delWrapper);
    }
}

} // namespace livim

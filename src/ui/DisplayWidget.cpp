#include "ui/DisplayWidget.hpp"

#include <algorithm>
#include <cmath>

#include <QLabel>
#include <QMouseEvent>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QRubberBand>
#include <QTimer>

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include "core/Instrumentation.hpp"
#include "core/LatestFrameMailbox.hpp"

namespace livim {
namespace {

// Fullscreen quad, clip-space position + texcoord; v is flipped so cv::Mat row 0 maps to screen top.
constexpr float kQuad[] = {
    // x     y     u    v
    -1.f, -1.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,
     1.f,  1.f, 1.f, 0.f,
};

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
out vec2 vTex;
void main() {
    vTex = aTex;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Colour textures hold BGR bytes uploaded as RGB, so the shader outputs .bgr to swizzle back;
// gray is a single-channel GL_R8 texture replicated across RGB. Requires GLSL 330 core.
constexpr char kFragmentShader[] = R"(#version 330 core
in vec2 vTex;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uGrayscale; // 1 = single-channel texture, 0 = BGR-in-RGB texture
void main() {
    if (uGrayscale == 1) {
        float g = texture(uTex, vTex).r;
        FragColor = vec4(g, g, g, 1.0);
    } else {
        FragColor = vec4(texture(uTex, vTex).bgr, 1.0);
    }
}
)";

} // namespace

DisplayWidget::DisplayWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    presentTimer_ = new QTimer(this);
    presentTimer_->setInterval(8); // ~120 Hz poll; vsync caps actual present rate
    connect(presentTimer_, &QTimer::timeout, this, [this] { update(); });

    // Mouse-transparent so ROI drags pass through to the widget.
    for (QLabel*& lbl : paneLabel_) {
        lbl = new QLabel(this);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        lbl->setStyleSheet("color: white; background-color: rgba(0,0,0,150);"
                           " padding: 2px 6px; border-radius: 3px; font-weight: bold;");
        lbl->hide();
    }
}

DisplayWidget::~DisplayWidget() {
    // GL deletes need a current context; context() is null if initializeGL never ran or after
    // context loss -- skip the deletes then (the driver already reclaimed the resources).
    if (context()) {
        makeCurrent();
        for (Tex* t : {&texProc_, &texOrig_}) {
            if (t->id != 0) {
                glDeleteTextures(1, &t->id);
                t->id = 0;
            }
        }
        vbo_.destroy();
        vao_.destroy();
        delete program_;
        program_ = nullptr;
        doneCurrent();
    }
}

void DisplayWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.f, 0.f, 0.f, 1.f);

    program_ = new QOpenGLShaderProgram(this);
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    program_->link();

    vao_.create();
    vao_.bind();

    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kQuad, sizeof(kQuad));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    vbo_.release();
    vao_.release();

    for (Tex* t : {&texProc_, &texOrig_}) {
        glGenTextures(1, &t->id);
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    presentTimer_->start();
}

void DisplayWidget::resizeGL(int /*w*/, int /*h*/) {
    updateRoiOverlayLabels();
}

void DisplayWidget::uploadFrame(const Frame& f, Tex& tex) {
    const cv::Mat& src = f.image;

    const int channels = src.channels();
    const GLint internalFormat = (channels == 1) ? GL_R8 : GL_RGB8;
    const GLenum pixelFormat = (channels == 1) ? GL_RED : GL_RGB;

    glBindTexture(GL_TEXTURE_2D, tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Handle a non-contiguous cv::Mat (row padding) via row length in pixels.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(src.step / src.elemSize()));

    if (f.width != tex.w || f.height != tex.h || channels != tex.channels) {
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, f.width, f.height, 0, pixelFormat,
                     GL_UNSIGNED_BYTE, src.data);
        tex.w = f.width;
        tex.h = f.height;
        tex.channels = channels;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height, pixelFormat, GL_UNSIGNED_BYTE,
                        src.data);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void DisplayWidget::drawTexture(const Tex& tex, int vx, int vy, int vw, int vh) {
    if (tex.w <= 0 || tex.h <= 0 || vw <= 0 || vh <= 0) return;

    const float frameAR = static_cast<float>(tex.w) / static_cast<float>(tex.h);
    const float regionAR = static_cast<float>(vw) / static_cast<float>(vh);
    int w = vw, h = vh;
    if (regionAR > frameAR) {
        h = vh;
        w = static_cast<int>(static_cast<float>(vh) * frameAR);
    } else {
        w = vw;
        h = static_cast<int>(static_cast<float>(vw) / frameAR);
    }
    glViewport(vx + (vw - w) / 2, vy + (vh - h) / 2, w, h);

    program_->bind();
    vao_.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    program_->setUniformValue("uTex", 0);
    program_->setUniformValue("uGrayscale", tex.channels == 1 ? 1 : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    vao_.release();
    program_->release();
}

int DisplayWidget::layoutPanes(Pane panes[2]) const {
    const double W = width();
    const double H = height();
    switch (viewMode_) {
    case ViewMode::Processed:
        panes[0] = {QRectF(0, 0, W, H), false};
        return 1;
    case ViewMode::Original:
        panes[0] = {QRectF(0, 0, W, H), true};
        return 1;
    case ViewMode::SideBySide: {
        const double half = W * 0.5;
        panes[0] = {QRectF(0, 0, half, H), true};          // original = left
        panes[1] = {QRectF(half, 0, W - half, H), false};  // processed = right
        return 2;
    }
    case ViewMode::Stacked: {
        const double half = H * 0.5;
        panes[0] = {QRectF(0, 0, W, half), true};          // original = top
        panes[1] = {QRectF(0, half, W, H - half), false};  // processed = bottom
        return 2;
    }
    }
    return 0;
}

void DisplayWidget::paintGL() {
    const qreal dpr = devicePixelRatioF();
    const int fbW = static_cast<int>(width() * dpr);
    const int fbH = static_cast<int>(height() * dpr);

    glViewport(0, 0, fbW, fbH);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto presentable = [](const FrameRef& f) { return f && !f->image.empty(); };

    const DisplayFrameRef df = mailbox_ ? mailbox_->latest() : nullptr;
    if (df && presentable(df->processed)) {
        const Frame& proc = *df->processed;
        // Both frames are published together; one seq check keeps the panes in lockstep.
        if (proc.seq != lastSeq_) {
            const bool needProc = (viewMode_ != ViewMode::Original);
            const bool needOrig = (viewMode_ != ViewMode::Processed);
            if (needProc) uploadFrame(proc, texProc_);
            if (needOrig && presentable(df->original)) uploadFrame(*df->original, texOrig_);

            if (flowOverlayMode_ != OpticalFlowOverlayMode::None && presentable(df->processed)) {
                cv::Mat currGray;
                if (df->processed->image.channels() == 1) {
                    currGray = df->processed->image;
                } else {
                    cv::cvtColor(df->processed->image, currGray, cv::COLOR_BGR2GRAY);
                }
                if (currGray.cols > 480) {
                    const double scale = 480.0 / currGray.cols;
                    cv::resize(currGray, currGray, cv::Size(), scale, scale, cv::INTER_LINEAR);
                }
                if (!lastProcGray_.empty() && lastProcGray_.size() == currGray.size()) {
                    cv::calcOpticalFlowFarneback(lastProcGray_, currGray, flowMat_, 0.5, 2, 12, 2, 5, 1.1, 0);
                }
                lastProcGray_ = currGray.clone();
            }

            if (instr_) {
                if (lastSeq_ != kNoSeq && proc.seq > lastSeq_ + 1)
                    instr_->addDisplaySkipped(proc.seq - lastSeq_ - 1);
                instr_->onDisplayed();
            }
            lastSeq_ = proc.seq;
        }
    }

    Pane panes[2];
    const int n = layoutPanes(panes);
    for (int i = 0; i < n; ++i) {
        const Tex& t = panes[i].original ? texOrig_ : texProc_;
        if (t.w <= 0) continue;
        const QRectF& r = panes[i].region;
        const int vx = static_cast<int>(std::lround(r.x() * dpr));
        const int vw = static_cast<int>(std::lround(r.width() * dpr));
        const int vh = static_cast<int>(std::lround(r.height() * dpr));
        const int vy = static_cast<int>(std::lround(fbH - (r.y() + r.height()) * dpr)); // GL y up
        drawTexture(t, vx, vy, vw, vh);
    }

    // Reposition the labels only on a layout change; doing it per frame churns the widgets.
    const int texW = texProc_.w > 0 ? texProc_.w : texOrig_.w;
    const int texH = texProc_.h > 0 ? texProc_.h : texOrig_.h;
    if (width() != lblSigW_ || height() != lblSigH_ || texW != lblSigTexW_ ||
        texH != lblSigTexH_ || static_cast<int>(viewMode_) != lblSigMode_) {
        updateLabels();
        lblSigW_ = width();
        lblSigH_ = height();
        lblSigTexW_ = texW;
        lblSigTexH_ = texH;
        lblSigMode_ = static_cast<int>(viewMode_);
    }
}

void DisplayWidget::updateLabels() {
    Pane panes[2];
    const int n = layoutPanes(panes);
    for (int i = 0; i < 2; ++i) {
        QLabel* lbl = paneLabel_[i];
        if (!lbl) continue;
        const bool shown = (i < n) && ((panes[i].original ? texOrig_.w : texProc_.w) > 0);
        if (!shown) {
            lbl->hide();
            continue;
        }
        const QString text = panes[i].original ? QStringLiteral("Original")
                                               : QStringLiteral("Processed");
        if (lbl->text() != text) lbl->setText(text);
        lbl->adjustSize();
        const QRectF img = letterboxRect(panes[i].region);
        lbl->move(static_cast<int>(img.left()) + 6, static_cast<int>(img.top()) + 6);
        lbl->show();
        lbl->raise();
    }
}

void DisplayWidget::setViewMode(ViewMode mode) {
    if (viewMode_ == mode) return;
    viewMode_ = mode;
    // Force a re-upload on the next paint so switching works while paused.
    lastSeq_ = kNoSeq;
    update();
}

QRectF DisplayWidget::letterboxRect(const QRectF& region) const {
    // All panes share geometry; use whichever texture is loaded for the aspect.
    const int tw = texProc_.w > 0 ? texProc_.w : texOrig_.w;
    const int th = texProc_.h > 0 ? texProc_.h : texOrig_.h;
    if (tw <= 0 || th <= 0 || region.width() <= 0.0 || region.height() <= 0.0) return region;
    const double frameAR = static_cast<double>(tw) / static_cast<double>(th);
    const double regionAR = region.width() / region.height();
    double cw = region.width(), ch = region.height();
    if (regionAR > frameAR) { ch = region.height(); cw = ch * frameAR; }
    else { cw = region.width(); ch = cw / frameAR; }
    return QRectF(region.x() + (region.width() - cw) * 0.5,
                  region.y() + (region.height() - ch) * 0.5, cw, ch);
}

QRectF DisplayWidget::paneImageRect(const QPointF& p) const {
    Pane panes[2];
    const int n = layoutPanes(panes);
    for (int i = 0; i < n; ++i)
        if (panes[i].region.contains(p)) return letterboxRect(panes[i].region);
    return n > 0 ? letterboxRect(panes[0].region) : QRectF(0, 0, width(), height());
}

void DisplayWidget::setRois(const std::vector<ROI>& rois) {
    rois_ = rois;
    updateRoiOverlayLabels();
    update();
}

void DisplayWidget::clearRois() {
    rois_.clear();
    updateRoiOverlayLabels();
    update();
}

void DisplayWidget::updateRoiOverlayLabels() {
    if (rois_.empty()) {
        for (QLabel* lbl : roiLabels_) {
            lbl->hide();
        }
        return;
    }

    Pane panes[2];
    const int numPanes = layoutPanes(panes);
    if (numPanes <= 0) {
        for (QLabel* lbl : roiLabels_) {
            lbl->hide();
        }
        return;
    }

    const QRectF c = letterboxRect(panes[0].region);
    if (c.width() < 1.0 || c.height() < 1.0) {
        for (QLabel* lbl : roiLabels_) {
            lbl->hide();
        }
        return;
    }

    std::size_t activeCount = 0;
    for (std::size_t i = 0; i < rois_.size(); ++i) {
        const ROI& roi = rois_[i];
        if (!roi.visible) continue;

        if (activeCount >= roiLabels_.size()) {
            auto* lbl = new QLabel(this);
            lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
            roiLabels_.push_back(lbl);
        }

        QLabel* lbl = roiLabels_[activeCount++];
        lbl->setText(QString::fromStdString(roi.name + " [" + motionAxisToString(roi.axis) + "]"));
        lbl->setStyleSheet(QString(
            "color: white; background-color: %1; padding: 1px 5px; "
            "border-radius: 3px; font-weight: bold; font-size: 11px;"
        ).arg(roi.color.name()));

        const double rx = c.left() + roi.normalizedRect.x() * c.width();
        const double ry = c.top() + roi.normalizedRect.y() * c.height();

        lbl->adjustSize();
        const int lblH = lbl->height();
        const int lblY = static_cast<int>(std::max(c.top(), ry - static_cast<double>(lblH)));
        lbl->move(static_cast<int>(rx), lblY);
        lbl->show();
        lbl->raise();
    }

    for (std::size_t i = activeCount; i < roiLabels_.size(); ++i) {
        roiLabels_[i]->hide();
    }
}

bool DisplayWidget::hitTestRoiHandles(const QPointF& pos, int& outRoiIdx, RoiHandle& outHandle) const {
    Pane panes[2];
    const int numPanes = layoutPanes(panes);
    if (numPanes <= 0) return false;

    constexpr double kHandleRadius = 8.0;

    for (int pIdx = 0; pIdx < numPanes; ++pIdx) {
        const QRectF c = letterboxRect(panes[pIdx].region);
        if (c.width() < 1.0 || c.height() < 1.0) continue;
        if (!c.contains(pos)) continue;

        for (int i = static_cast<int>(rois_.size()) - 1; i >= 0; --i) {
            const ROI& roi = rois_[i];
            if (!roi.visible) continue;

            const double rx = c.left() + roi.normalizedRect.x() * c.width();
            const double ry = c.top() + roi.normalizedRect.y() * c.height();
            const double rw = roi.normalizedRect.width() * c.width();
            const double rh = roi.normalizedRect.height() * c.height();

            const QPointF tl(rx, ry);
            const QPointF tr(rx + rw, ry);
            const QPointF bl(rx, ry + rh);
            const QPointF br(rx + rw, ry + rh);

            auto hitRect = [](const QPointF& pt, double r) {
                return QRectF(pt.x() - r, pt.y() - r, r * 2.0, r * 2.0);
            };

            if (hitRect(tl, kHandleRadius).contains(pos)) { outRoiIdx = i; outHandle = RoiHandle::TopLeft; return true; }
            if (hitRect(tr, kHandleRadius).contains(pos)) { outRoiIdx = i; outHandle = RoiHandle::TopRight; return true; }
            if (hitRect(bl, kHandleRadius).contains(pos)) { outRoiIdx = i; outHandle = RoiHandle::BottomLeft; return true; }
            if (hitRect(br, kHandleRadius).contains(pos)) { outRoiIdx = i; outHandle = RoiHandle::BottomRight; return true; }

            if (QRectF(rx, ry, rw, rh).contains(pos)) { outRoiIdx = i; outHandle = RoiHandle::Center; return true; }
        }
    }
    return false;
}

void DisplayWidget::computeAndDrawOpticalFlow(QPainter& painter, const QRectF& c) {
    if (flowOverlayMode_ == OpticalFlowOverlayMode::None || flowMat_.empty()) return;

    const int cols = flowMat_.cols;
    const int rows = flowMat_.rows;
    if (cols <= 0 || rows <= 0) return;

    if (flowOverlayMode_ == OpticalFlowOverlayMode::VectorGrid) {
        constexpr int step = 16;
        for (int y = step / 2; y < rows; y += step) {
            for (int x = step / 2; x < cols; x += step) {
                const cv::Point2f flow = flowMat_.at<cv::Point2f>(y, x);
                const double mag = std::sqrt(flow.x * flow.x + flow.y * flow.y);
                if (mag < 0.15) continue;

                const double sx = c.left() + (static_cast<double>(x) / cols) * c.width();
                const double sy = c.top() + (static_cast<double>(y) / rows) * c.height();

                const double ex = sx + (flow.x / cols) * c.width() * 4.0;
                const double ey = sy + (flow.y / rows) * c.height() * 4.0;

                const int hue = std::clamp(static_cast<int>(200.0 - mag * 25.0), 0, 200);
                QColor color = QColor::fromHsv(hue, 230, 255);

                painter.setPen(QPen(color, 1.5));
                painter.drawLine(QPointF(sx, sy), QPointF(ex, ey));

                const double angle = std::atan2(ey - sy, ex - sx);
                constexpr double arrowSize = 4.0;
                const QPointF p1(ex - arrowSize * std::cos(angle - M_PI / 6.0),
                                 ey - arrowSize * std::sin(angle - M_PI / 6.0));
                const QPointF p2(ex - arrowSize * std::cos(angle + M_PI / 6.0),
                                 ey - arrowSize * std::sin(angle + M_PI / 6.0));
                painter.drawLine(QPointF(ex, ey), p1);
                painter.drawLine(QPointF(ex, ey), p2);
            }
        }
    } else if (flowOverlayMode_ == OpticalFlowOverlayMode::Heatmap) {
        cv::Mat magMat(rows, cols, CV_32FC1);
        for (int r = 0; r < rows; ++r) {
            const cv::Point2f* fPtr = flowMat_.ptr<cv::Point2f>(r);
            float* mPtr = magMat.ptr<float>(r);
            for (int cIdx = 0; cIdx < cols; ++cIdx) {
                mPtr[cIdx] = std::sqrt(fPtr[cIdx].x * fPtr[cIdx].x + fPtr[cIdx].y * fPtr[cIdx].y);
            }
        }
        cv::Mat mag8u;
        magMat.convertTo(mag8u, CV_8UC1, 30.0);
        cv::Mat colorMap;
        cv::applyColorMap(mag8u, colorMap, cv::COLORMAP_TURBO);

        cv::Mat bgraMap;
        cv::cvtColor(colorMap, bgraMap, cv::COLOR_BGR2BGRA);
        for (int r = 0; r < rows; ++r) {
            cv::Vec4b* ptr = bgraMap.ptr<cv::Vec4b>(r);
            const float* mPtr = magMat.ptr<float>(r);
            for (int cIdx = 0; cIdx < cols; ++cIdx) {
                const uchar alpha = static_cast<uchar>(std::clamp(mPtr[cIdx] * 40.0, 0.0, 160.0));
                ptr[cIdx][3] = alpha;
            }
        }

        QImage qImg(bgraMap.data, bgraMap.cols, bgraMap.rows, static_cast<int>(bgraMap.step), QImage::Format_ARGB32);
        painter.setOpacity(0.75);
        painter.drawImage(c, qImg);
        painter.setOpacity(1.0);
    }
}

void DisplayWidget::paintEvent(QPaintEvent* e) {
    QOpenGLWidget::paintEvent(e);

    updateRoiOverlayLabels();

    Pane panes[2];
    const int numPanes = layoutPanes(panes);
    if (numPanes <= 0) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (flowOverlayMode_ != OpticalFlowOverlayMode::None && !flowMat_.empty()) {
        for (int pIdx = 0; pIdx < numPanes; ++pIdx) {
            const QRectF c = letterboxRect(panes[pIdx].region);
            if (c.width() < 1.0 || c.height() < 1.0) continue;
            computeAndDrawOpticalFlow(painter, c);
        }
    }

    if (!rois_.empty()) {
        for (int pIdx = 0; pIdx < numPanes; ++pIdx) {
            const QRectF c = letterboxRect(panes[pIdx].region);
            if (c.width() < 1.0 || c.height() < 1.0) continue;

            for (const ROI& roi : rois_) {
                if (!roi.visible) continue;

                const double rx = c.left() + roi.normalizedRect.x() * c.width();
                const double ry = c.top() + roi.normalizedRect.y() * c.height();
                const double rw = roi.normalizedRect.width() * c.width();
                const double rh = roi.normalizedRect.height() * c.height();

                const QRectF box(rx, ry, rw, rh);

                QPen pen(roi.color, 2, Qt::SolidLine);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(box);

                // Draw 4 corner control handles for interactive resizing
                constexpr double hSize = 6.0;
                painter.setBrush(Qt::white);
                painter.setPen(QPen(roi.color, 1.5));
                painter.drawRect(QRectF(rx - hSize / 2.0, ry - hSize / 2.0, hSize, hSize));
                painter.drawRect(QRectF(rx + rw - hSize / 2.0, ry - hSize / 2.0, hSize, hSize));
                painter.drawRect(QRectF(rx - hSize / 2.0, ry + rh - hSize / 2.0, hSize, hSize));
                painter.drawRect(QRectF(rx + rw - hSize / 2.0, ry + rh - hSize / 2.0, hSize, hSize));
            }
        }
    }
}

void DisplayWidget::setRoiMode(RoiMode mode) {
    roiMode_ = mode;
    if (mode == RoiMode::SingleProcessing || mode == RoiMode::MultiRoiAdd) {
        setCursor(Qt::CrossCursor);
    } else {
        setCursor(Qt::ArrowCursor);
        if (rubberBand_) rubberBand_->hide();
    }
}

void DisplayWidget::setRoiDrawingEnabled(bool enabled) {
    setRoiMode(enabled ? RoiMode::SingleProcessing : RoiMode::None);
}

void DisplayWidget::setOpticalFlowOverlayMode(OpticalFlowOverlayMode mode) {
    if (flowOverlayMode_ == mode) return;
    flowOverlayMode_ = mode;
    update();
}

void DisplayWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QOpenGLWidget::mousePressEvent(e);
        return;
    }

    if (roiMode_ == RoiMode::SingleProcessing || roiMode_ == RoiMode::MultiRoiAdd) {
        if (texProc_.w <= 0 && texOrig_.w <= 0) {
            QOpenGLWidget::mousePressEvent(e);
            return;
        }
        roiDrawRect_ = paneImageRect(e->position());
        const QRectF c = roiDrawRect_;
        const QPointF p = e->position();
        roiOrigin_ = QPoint(static_cast<int>(std::clamp(p.x(), c.left(), c.right())),
                            static_cast<int>(std::clamp(p.y(), c.top(), c.bottom())));
        if (!rubberBand_) rubberBand_ = new QRubberBand(QRubberBand::Rectangle, this);
        rubberBand_->setGeometry(QRect(roiOrigin_, QSize()));
        rubberBand_->show();
        return;
    }

    if (roiMode_ == RoiMode::None && !rois_.empty()) {
        int roiIdx = -1;
        RoiHandle handle = RoiHandle::None;
        if (hitTestRoiHandles(e->position(), roiIdx, handle)) {
            activeRoiIndex_ = roiIdx;
            activeHandle_ = handle;
            isDraggingRoi_ = true;
            dragStartPos_ = e->position();
            dragStartRect_ = rois_[roiIdx].normalizedRect;
            dragPaneRect_ = paneImageRect(e->position());
            return;
        }
    }

    QOpenGLWidget::mousePressEvent(e);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent* e) {
    if (roiMode_ == RoiMode::SingleProcessing || roiMode_ == RoiMode::MultiRoiAdd) {
        if (rubberBand_ && rubberBand_->isVisible()) {
            const QRectF c = roiDrawRect_;
            const QPointF p = e->position();
            const QPoint cur(static_cast<int>(std::clamp(p.x(), c.left(), c.right())),
                             static_cast<int>(std::clamp(p.y(), c.top(), c.bottom())));
            rubberBand_->setGeometry(QRect(roiOrigin_, cur).normalized());
            return;
        }
    } else if (isDraggingRoi_ && activeRoiIndex_ >= 0 && activeRoiIndex_ < static_cast<int>(rois_.size())) {
        const QPointF p = e->position();
        const double dx = p.x() - dragStartPos_.x();
        const double dy = p.y() - dragStartPos_.y();
        const double paneW = dragPaneRect_.width();
        const double paneH = dragPaneRect_.height();

        if (paneW > 1.0 && paneH > 1.0) {
            const double ndx = dx / paneW;
            const double ndy = dy / paneH;
            QRectF rect = dragStartRect_;

            constexpr double kMinSize = 0.01;

            if (activeHandle_ == RoiHandle::Center) {
                double nx = rect.x() + ndx;
                double ny = rect.y() + ndy;
                nx = std::clamp(nx, 0.0, 1.0 - rect.width());
                ny = std::clamp(ny, 0.0, 1.0 - rect.height());
                rect.moveTo(nx, ny);
            } else {
                double left = rect.left();
                double top = rect.top();
                double right = rect.right();
                double bottom = rect.bottom();

                switch (activeHandle_) {
                case RoiHandle::TopLeft:
                    left = std::clamp(rect.left() + ndx, 0.0, right - kMinSize);
                    top = std::clamp(rect.top() + ndy, 0.0, bottom - kMinSize);
                    break;
                case RoiHandle::TopRight:
                    right = std::clamp(rect.right() + ndx, left + kMinSize, 1.0);
                    top = std::clamp(rect.top() + ndy, 0.0, bottom - kMinSize);
                    break;
                case RoiHandle::BottomLeft:
                    left = std::clamp(rect.left() + ndx, 0.0, right - kMinSize);
                    bottom = std::clamp(rect.bottom() + ndy, top + kMinSize, 1.0);
                    break;
                case RoiHandle::BottomRight:
                    right = std::clamp(rect.right() + ndx, left + kMinSize, 1.0);
                    bottom = std::clamp(rect.bottom() + ndy, top + kMinSize, 1.0);
                    break;
                default:
                    break;
                }
                rect = QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
            }

            rois_[activeRoiIndex_].normalizedRect = rect;
            updateRoiOverlayLabels();
            update();
        }
        return;
    } else if (roiMode_ == RoiMode::None && !rois_.empty()) {
        int roiIdx = -1;
        RoiHandle handle = RoiHandle::None;
        if (hitTestRoiHandles(e->position(), roiIdx, handle)) {
            switch (handle) {
            case RoiHandle::TopLeft:
            case RoiHandle::BottomRight:
                setCursor(Qt::SizeFDiagCursor);
                break;
            case RoiHandle::TopRight:
            case RoiHandle::BottomLeft:
                setCursor(Qt::SizeBDiagCursor);
                break;
            case RoiHandle::Center:
                setCursor(Qt::SizeAllCursor);
                break;
            default:
                setCursor(Qt::ArrowCursor);
                break;
            }
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }

    QOpenGLWidget::mouseMoveEvent(e);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseReleaseEvent(e);
        return;
    }

    if (isDraggingRoi_) {
        isDraggingRoi_ = false;
        activeRoiIndex_ = -1;
        activeHandle_ = RoiHandle::None;
        emit roisUpdated(rois_);
        return;
    }

    if ((roiMode_ == RoiMode::SingleProcessing || roiMode_ == RoiMode::MultiRoiAdd) && rubberBand_) {
        const QRect band = rubberBand_->geometry();
        rubberBand_->hide();
        const QRectF c = roiDrawRect_;
        if (c.width() < 1.0 || c.height() < 1.0 || band.width() < 6 || band.height() < 6) return;

        float x = static_cast<float>((band.left() - c.left()) / c.width());
        float y = static_cast<float>((band.top() - c.top()) / c.height());
        float w = static_cast<float>(band.width() / c.width());
        float h = static_cast<float>(band.height() / c.height());
        x = std::clamp(x, 0.0f, 1.0f);
        y = std::clamp(y, 0.0f, 1.0f);
        w = std::clamp(w, 0.0f, 1.0f - x);
        h = std::clamp(h, 0.0f, 1.0f - y);
        if (w <= 0.0f || h <= 0.0f) return;

        if (roiMode_ == RoiMode::SingleProcessing) {
            emit roiSelected(x, y, w, h);
        } else if (roiMode_ == RoiMode::MultiRoiAdd) {
            emit roiCreated(QRectF(x, y, w, h));
        }
        return;
    }

    QOpenGLWidget::mouseReleaseEvent(e);
}

} // namespace livim

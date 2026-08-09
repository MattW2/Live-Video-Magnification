#include "pipeline/PlaybackController.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "export/FileExportFrameSource.hpp"
#include "export/RecordingBuffer.hpp"
#include "processing/ChainBuilder.hpp"
#include "processing/ProcessingChain.hpp"
#include "processing/magnification/SpatialFilter.hpp"
#include "source/CameraSource.hpp"
#include "source/FileSource.hpp"
#include "core/Clock.hpp"
#include "core/IVideoRenderer.hpp"

namespace livim {

PlaybackController::PlaybackController()
    : pool_(kPoolCapacity), queue_(kQueueCapacity) {}

PlaybackController::~PlaybackController() {
    teardownThreads();
    // Unbind the renderer from the about-to-be-destroyed mailbox, independent of widget-vs-
    // controller destruction order. Worker threads are already joined.
    if (renderer_) renderer_->bindMailbox(nullptr);
}

void PlaybackController::bindRenderer(IVideoRenderer* renderer) {
    std::lock_guard<std::mutex> lg(mu_);
    renderer_ = renderer;
    if (renderer_) renderer_->bindMailbox(&mailbox_);
}

bool PlaybackController::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lg(mu_);
    currentFilePath_ = path;
    playbackFps_ = 0.0; // follow the source's reported FPS until overridden
    cameraSource_ = false;
    factory_ = [this, path] {
        return std::make_unique<FileSource>(path, &queue_, &pool_, &instr_);
    };
    teardownThreads();
    if (!buildAndStart()) { // bad path / unsupported codec
        factory_ = nullptr;
        state_ = State::Idle;
        return false;
    }
    state_ = State::Stopped; // loaded and paused at the start; play() begins playback
    return true;
}

bool PlaybackController::openCamera(int deviceIndex) {
    std::lock_guard<std::mutex> lg(mu_);
    playbackFps_ = 0.0; // follow the source's reported FPS until overridden
    cameraSource_ = true;
    factory_ = [this, deviceIndex] {
        return std::make_unique<CameraSource>(deviceIndex, &queue_, &pool_, &instr_);
    };
    teardownThreads();
    if (!buildAndStart()) {
        factory_ = nullptr;
        state_ = State::Idle;
        return false;
    }
    state_ = State::Stopped;
    return true;
}

bool PlaybackController::buildAndStart() {
    if (!factory_) return false;

    // Re-arm shared infrastructure for a fresh run (threads must already be torn down).
    queue_.reset();
    // Camera: Drop, so the grab loop never stalls. File: Block, so no frame is ever lost.
    queue_.setPolicy(cameraSource_ ? OverflowPolicy::Drop : OverflowPolicy::Block);
    pool_.reset();
    instr_.reset();
    mailbox_.clear();

    std::unique_ptr<ISource> source = factory_();
    if (!source->open()) return false;

    // Same processor list the Exporter uses, so live preview and export can never diverge.
    std::vector<std::unique_ptr<IProcessor>> procs = buildProcessors();

    chain_ = std::make_unique<ProcessingChain>(&queue_, &mailbox_, &instr_, &config_);
    chain_->setProcessors(std::move(procs));

    source_ = std::move(source);
    source_->setLoop(loop_);

    // Seed the playback cadence before the thread starts so the first frame is paced correctly.
    reportedFps_ = source_->reportedFps();
    source_->setPlaybackFps(playbackFps_ > 0.0 ? playbackFps_ : reportedFps_);

    // magParams_.framerate is owned by the panel's Capture FPS and must survive a source rebuild.
    if (magParams_.framerate <= 0.0) magParams_.framerate = reportedFps_ > 0.0 ? reportedFps_ : 30.0;
    config_.publish(composeConfig());

    // Start the consumer before the producer so no frame waits on an unstarted chain.
    chain_->start();
    source_->start(); // begins paused
    return true;
}

void PlaybackController::play() {
    std::lock_guard<std::mutex> lg(mu_);
    if (!factory_) return;

    if (source_ && !source_->finished()) {
        // A file parked at its end: restart from the beginning on Play.
        if (source_->atEnd()) {
            source_->seekFrame(0);
            source_->play();
            state_ = State::Playing;
            return;
        }
        if (state_ != State::Playing) {
            source_->play();
            state_ = State::Playing;
        }
        return;
    }

    // Fresh source needed: after Stop (threads torn down) or a dead live source.
    teardownThreads();
    if (!buildAndStart()) {
        state_ = State::Idle;
        return;
    }
    source_->play();
    state_ = State::Playing;
}

void PlaybackController::pause() {
    std::lock_guard<std::mutex> lg(mu_);
    if (source_ && !source_->finished() && state_ == State::Playing) {
        source_->pause();
        state_ = State::Paused;
    }
}

void PlaybackController::stop() {
    std::lock_guard<std::mutex> lg(mu_);
    // Seekable source (file): stay loaded but rewind and pause, so the timeline stays scrubbable
    // while stopped. Camera / finished sources are torn down; the next Play rebuilds from factory_.
    if (source_ && !source_->finished() && source_->seekable()) {
        source_->pause();
        source_->seekFrame(0);
        state_ = State::Stopped;
        return;
    }
    teardownThreads();
    state_ = State::Stopped;
}

bool PlaybackController::isPlaying() {
    std::lock_guard<std::mutex> lg(mu_);
    return state_ == State::Playing && source_ && !source_->finished() && !source_->atEnd();
}

void PlaybackController::setLoop(bool enabled) {
    std::lock_guard<std::mutex> lg(mu_);
    loop_ = enabled;
    if (source_) source_->setLoop(enabled);
}

ProcessorConfig PlaybackController::composeConfig() const {
    ProcessorConfig cfg;
    cfg.grayscale = grayscale_;
    cfg.preprocess = preprocess_;
    cfg.magnification = magParams_;
    // Original-only view: bypass magnification entirely -- its output isn't displayed.
    if (!magnifyActive_) cfg.magnification.mode = MagnificationMode::None;
    return cfg;
}

void PlaybackController::setGrayscale(bool enabled) {
    mutateConfig([&] { grayscale_ = enabled; });
}

bool PlaybackController::grayscaleEnabled() {
    std::lock_guard<std::mutex> lg(mu_);
    return grayscale_;
}

void PlaybackController::setMagnification(const MagnificationParams& params) {
    mutateConfig([&] {
        magParams_ = params;
        if (magParams_.framerate <= 0.0) magParams_.framerate = reportedFps_ > 0.0 ? reportedFps_ : 30.0;
    });
}

void PlaybackController::setMagnificationActive(bool active) {
    mutateConfig([&] { magnifyActive_ = active; });
}

MagnificationParams PlaybackController::magnification() {
    std::lock_guard<std::mutex> lg(mu_);
    return magParams_;
}

int PlaybackController::maxPyramidLevels() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ ? calculateMaxLevels(source_->nativeSize()) : 0;
}

void PlaybackController::setDownscale(int divisor) {
    mutateConfig([&] { preprocess_.downscale = std::clamp(divisor, 1, 8); });
}

void PlaybackController::setRoi(float x, float y, float w, float h) {
    // (x,y,w,h) are normalized to the CURRENTLY displayed image, so compose onto an active ROI to
    // keep it relative to the full source frame. New origin must use the OLD size.
    mutateConfig([&] {
        if (preprocess_.roiEnabled) {
            preprocess_.roiX += x * preprocess_.roiW;
            preprocess_.roiY += y * preprocess_.roiH;
            preprocess_.roiW *= w;
            preprocess_.roiH *= h;
        } else {
            preprocess_.roiEnabled = true;
            preprocess_.roiX = x;
            preprocess_.roiY = y;
            preprocess_.roiW = w;
            preprocess_.roiH = h;
        }
    });
}

void PlaybackController::clearRoi() {
    mutateConfig([&] {
        preprocess_.roiEnabled = false;
        preprocess_.roiX = 0.0f;
        preprocess_.roiY = 0.0f;
        preprocess_.roiW = 1.0f;
        preprocess_.roiH = 1.0f;
    });
}

PreprocessParams PlaybackController::preprocess() {
    std::lock_guard<std::mutex> lg(mu_);
    return preprocess_;
}

void PlaybackController::beginCameraRecording(std::shared_ptr<RecordingBuffer> buffer) {
    std::lock_guard<std::mutex> lg(mu_);
    if (!source_ || source_->kind() != SourceKind::Camera) return;
    recordBuf_ = std::move(buffer);
    source_->setRecordTarget(recordBuf_, &mailbox_);
    source_->play(); // make sure the camera is actually grabbing
    state_ = State::Playing;
}

void PlaybackController::endCameraRecording() {
    std::lock_guard<std::mutex> lg(mu_);
    // Order matters: reject late appends FIRST, then quiesce the producer, then detach the target.
    if (recordBuf_) recordBuf_->close();
    if (source_) {
        source_->pause();
        source_->clearRecordTarget();
    }
    state_ = State::Paused;
    recordBuf_.reset(); // the caller still holds its own reference for takeFrames()
}

int PlaybackController::sourceChannels() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ ? source_->nativeChannels() : 0;
}

double PlaybackController::reportedFps() {
    std::lock_guard<std::mutex> lg(mu_);
    return reportedFps_;
}

void PlaybackController::setPlaybackFps(double fps) {
    std::lock_guard<std::mutex> lg(mu_);
    playbackFps_ = fps;
    if (source_) source_->setPlaybackFps(fps);
}

bool PlaybackController::seekable() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ && source_->seekable();
}

std::int64_t PlaybackController::frameCount() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ ? source_->frameCount() : 0;
}

std::int64_t PlaybackController::currentFrame() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ ? source_->currentFrame() : 0;
}

void PlaybackController::seekFrame(std::int64_t frame) {
    std::lock_guard<std::mutex> lg(mu_);
    if (source_) source_->seekFrame(frame);
}

void PlaybackController::setInOut(std::int64_t in, std::int64_t out) {
    std::lock_guard<std::mutex> lg(mu_);
    if (source_) source_->setInOut(in, out);
}

double PlaybackController::playbackFps() {
    std::lock_guard<std::mutex> lg(mu_);
    return playbackFps_ > 0.0 ? playbackFps_ : reportedFps_;
}

bool PlaybackController::atEnd() {
    std::lock_guard<std::mutex> lg(mu_);
    return source_ && source_->atEnd();
}

void PlaybackController::teardownThreads() {
    // Order matters for deadlock-freedom: unblock the queue and pool FIRST so a producer that
    // is blocked on push()/acquire() (backpressure) can return before we join its thread.
    queue_.stop();
    pool_.stop();

    if (source_) {
        source_->stop();
        source_.reset();
    }
    if (chain_) {
        chain_->stop();
        chain_.reset();
    }
    mailbox_.clear();
}

StatsSnapshot PlaybackController::stats() {
    instr_.setSourceDrops(queue_.drops());
    instr_.setQueueDepth(queue_.size());
    return instr_.snapshot();
}

double PlaybackController::getSystemRamLoadPercent() const {
#if defined(_WIN32)
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex)) {
        return static_cast<double>(statex.dwMemoryLoad);
    }
#endif
    return 0.0;
}

bool PlaybackController::hasRamCache() const {
    return ramCache_.isComplete();
}

bool PlaybackController::hasScratchCache() const {
    return scratchCache_.isComplete();
}

void PlaybackController::clearCache() {
    ramCache_.clear();
    scratchCache_.cleanup();
}

std::size_t PlaybackController::ramCacheMemoryBytes() const {
    return ramCache_.memoryUsageBytes();
}

void PlaybackController::setSpeedMultiplier(double multiplier) {
    speedMultiplier_ = std::max(0.1, multiplier);
    if (source_) {
        const double baseFps = playbackFps_ > 0.0 ? playbackFps_ : reportedFps_;
        source_->setPlaybackFps(baseFps * speedMultiplier_);
    }
}

bool PlaybackController::precomputeCache(std::int64_t inFrame, std::int64_t outFrame,
                                          std::function<bool(int done, int total, bool isScratchFile)> progressCb) {
    if (currentFilePath_.empty() || cameraSource_) return false;

    clearCache();
    const bool wasPlaying = isPlaying();
    pause();

    FileExportFrameSource src(currentFilePath_, inFrame, outFrame);
    if (!src.open()) {
        if (wasPlaying) play();
        return false;
    }

    const double fps = reportedFps_ > 0.0 ? reportedFps_ : 30.0;
    const std::int64_t totalFrames = src.frameCount();
    if (totalFrames <= 0) {
        src.close();
        if (wasPlaying) play();
        return false;
    }

    const ProcessorConfig cfg = composeConfig();
    std::vector<std::unique_ptr<IProcessor>> chain = buildProcessors();

    double ramLoad = getSystemRamLoadPercent();
    bool isScratch = (ramLoad >= 80.0);

    if (isScratch) {
        std::filesystem::path scratchPath = std::filesystem::temp_directory_path() / "livim_scratch_cache.avi";
        cv::Mat sampleRaw;
        if (src.next(sampleRaw) && !sampleRaw.empty()) {
            src.close();
            src.open();
            scratchCache_.open(scratchPath.string(), fps, sampleRaw.size());
        }
    } else {
        ramCache_.setRangeAndFps(inFrame, inFrame + totalFrames, fps);
        ramCache_.setConfig(cfg);
    }

    std::uint64_t seq = 0;
    const double frameIntervalUs = 1'000'000.0 / (fps > 0.0 ? fps : 30.0);
    cv::Mat raw;
    int doneCount = 0;

    while (src.next(raw)) {
        if (raw.empty()) continue;

        auto in = std::make_shared<Frame>();
        in->seq = seq++;
        in->captureTs = now();
        in->ptsUs = static_cast<std::int64_t>(static_cast<double>(in->seq) * frameIntervalUs);
        in->width = raw.cols;
        in->height = raw.rows;
        in->format = raw.channels() == 1 ? PixelFormat::Gray8 : PixelFormat::BGR8;
        in->image = raw;

        FrameRef original;
        const FrameRef proc = runChainOnce(chain, in, cfg, original);

        if (!isScratch) {
            if (getSystemRamLoadPercent() >= 80.0) {
                isScratch = true;
                std::filesystem::path scratchPath = std::filesystem::temp_directory_path() / "livim_scratch_cache.avi";
                scratchCache_.open(scratchPath.string(), fps, proc->image.size());
                for (std::size_t i = 0; i < ramCache_.frameCount(); ++i) {
                    cv::Mat pMat, oMat;
                    if (ramCache_.getFrame(inFrame + static_cast<std::int64_t>(i), pMat, oMat)) {
                        scratchCache_.writeFrame(pMat);
                    }
                }
                ramCache_.clear();
            }
        }

        if (isScratch) {
            scratchCache_.writeFrame(proc->image);
        } else {
            ramCache_.addFrame(proc->image, original->image);
        }

        doneCount++;
        if (progressCb && !progressCb(doneCount, static_cast<int>(totalFrames), isScratch)) {
            src.close();
            clearCache();
            if (wasPlaying) play();
            return false;
        }
    }

    src.close();

    if (isScratch) {
        scratchCache_.finalize();
        openFile(scratchCache_.path());
    } else {
        ramCache_.setComplete(true);
    }

    if (wasPlaying) play();
    return true;
}

} // namespace livim

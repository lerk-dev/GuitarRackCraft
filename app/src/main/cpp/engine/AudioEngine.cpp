/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include "AudioEngine.h"
#include "utils/WavIO.h"
#include "utils/ThreadUtils.h"
#include <oboe/OboeExtensions.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <algorithm>
#include <thread>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

AudioEngine::AudioEngine()
    : sampleRate_(48000.0f)
    , isRunning_(false)
{
    inputPtrs_[0] = nullptr;
    inputPtrs_[1] = nullptr;
    outputPtrs_[0] = nullptr;
    outputPtrs_[1] = nullptr;
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start(float sampleRate, int32_t inputDeviceId,
                        int32_t outputDeviceId, int32_t bufferFrames) {
    LOGI("start() ENTER tid=%ld sampleRate=%.0f inputDev=%d outputDev=%d bufFrames=%d isRunning_=%d",
         getTid(), sampleRate, inputDeviceId, outputDeviceId, bufferFrames, isRunning_ ? 1 : 0);
    if (isRunning_) {
        LOGI("start() early return (already running)");
        return true;
    }

    sampleRate_ = sampleRate;
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    requestedBufferFrames_ = bufferFrames;

    if (!createAudioStreams(sampleRate)) {
        LOGE("Failed to create audio streams");
        return false;
    }

    // Activate plugin chain with the power-of-2 callback frame count
    // so convolver plugins configure their partition size correctly
    LOGI("Using callback frame count: %u (power-of-2)", callbackFrameCount_);
    chain_.setSampleRate(sampleRate_, callbackFrameCount_);
    chain_.activate();

    isRunning_ = true;
    LOGI("start() EXIT tid=%ld Audio engine started at %.0f Hz", getTid(), sampleRate_);
    return true;
}

oboe::Result AudioEngine::stop() {
    LOGI("stop() entered tid=%ld isRunning_=%d", getTid(), isRunning_ ? 1 : 0);
    if (!isRunning_) {
        // Stream may have been closed by onErrorAfterClose (e.g. system closed stream when opening X11 UI).
        // We must still call closeStreams() so streams are torn down with the 250ms wait before the
        // destructor runs. Otherwise ~AudioEngine destroys outputStream_/inputStream_ while the
        // AudioTrack callback thread is still in getStream() -> pthread_mutex_lock on destroyed mutex (SIGABRT).
        LOGI("stop() isRunning_=0; calling closeStreams() anyway so streams tear down safely");
        closeStreams();
        return oboe::Result::OK;
    }
    // Stop recording before tearing down the audio path
    if (recorder_.isRecording()) {
        recorder_.stopRecording();
    }

    // Signal callback to exit immediately so it does not touch chain_ or stream
    // during teardown (avoids use-after-free / destroyed mutex in plugin chain or Oboe).
    isRunning_ = false;
    LOGI("stop() isRunning_=false set, calling chain_.deactivate()");

    chain_.deactivate();
    LOGI("stop() chain_.deactivate() done, calling closeStreams()");

    closeStreams();
    LOGI("stop() done");
    return oboe::Result::OK;
}

bool AudioEngine::isRunning() const {
    return isRunning_;
}

AudioEngine::StreamInfo AudioEngine::getStreamInfo() const {
    StreamInfo info;
    if (outputStream_) {
        info.isAAudio = outputStream_->getAudioApi() == oboe::AudioApi::AAudio;
        info.outputExclusive = outputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.outputLowLatency = outputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
        info.outputMMap = oboe::OboeExtensions::isMMapUsed(outputStream_.get());
        info.outputCallback = true; // always using callback
        info.framesPerBurst = outputStream_->getFramesPerBurst();
    }
    if (inputStream_) {
        info.inputExclusive = inputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.inputLowLatency = inputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
    }
    return info;
}

double AudioEngine::getLatencyMs() const {
    double latencyMs = 0.0;

    // Output stream: buffer size + frames written-but-not-yet-played
    if (outputStream_) {
        int64_t framesWritten = outputStream_->getFramesWritten();
        int64_t framesRead = outputStream_->getFramesRead();
        int32_t bufferSize = outputStream_->getBufferSizeInFrames();

        double latencyFrames = bufferSize + (framesWritten - framesRead);
        latencyMs += (latencyFrames / sampleRate_) * 1000.0;
    }

    // Input stream: the input buffer is the dominant latency source on devices
    // where the audio policy limits the capture buffer (e.g. 4096 frames ≈ 85ms
    // at 48kHz). Ignoring it makes the reported latency wildly optimistic.
    if (inputStream_) {
        int32_t inBufferSize = inputStream_->getBufferSizeInFrames();
        if (inBufferSize > 0) {
            latencyMs += (inBufferSize / sampleRate_) * 1000.0;
        }
    }

    return latencyMs;
}

float AudioEngine::getInputLevel() const {
    return inputPeakLevel_.load();
}

float AudioEngine::getOutputLevel() const {
    return outputPeakLevel_.load();
}

float AudioEngine::getCpuLoad() const {
    /* Audio-thread load (cpuLoad_) measures chain.process() wall time vs
     * buffer duration. For LV2 plugins (in-process), that's the real
     * processing cost. For VST plugins (out-of-process via wine), it
     * measures only the shm push/pull — the wine subprocess does the
     * actual work on different threads in another address space. Reporting
     * the max of the two captures the bottleneck either way. */
    return std::max(cpuLoad_.load(), vstCpuLoad_.load());
}

int32_t AudioEngine::getXRunCount() const {
    int32_t oboeXruns = 0;
    if (outputStream_) {
        auto result = outputStream_->getXRunCount();
        oboeXruns = result ? result.value() : 0;
    }
    /* Oboe-side xruns only happen when the audio thread itself misses a
     * deadline. VST plugins under wine zero-fill on their own (the audio
     * thread sees full buffers, just silent) — so Oboe never knows. Add
     * the per-plugin underrun counter so the UI's "xruns" reflects both. */
    return oboeXruns + vstUnderruns_.load();
}

int32_t AudioEngine::getOutputSessionId() const {
    if (!outputStream_) {
        return 0;
    }
    int32_t sessionId = outputStream_->getSessionId();
    // Oboe returns kNoSessionId (-1) when the stream has no session.
    return sessionId < 0 ? 0 : sessionId;
}

bool AudioEngine::isInputClipping() const {
    return inputClipping_.load();
}

bool AudioEngine::isOutputClipping() const {
    return outputClipping_.load();
}

void AudioEngine::resetClipping() {
    inputClipping_.store(false);
    outputClipping_.store(false);
}

// --- WAV playback ---

bool AudioEngine::loadWav(const std::string& path) {
    if (!isRunning_) {
        LOGE("loadWav: engine not running");
        return false;
    }
    wavPause();
    std::vector<float> samples;
    uint32_t fileRate = 44100;
    uint32_t numChannels = 1;
    if (!guitarrackcraft::readWavFile(path, samples, fileRate, numChannels)) {
        return false;
    }
    // Convert to mono if stereo (samples are interleaved L,R,L,R,...)
    std::vector<float> mono;
    if (numChannels == 2) {
        size_t numFrames = samples.size() / 2;
        mono.resize(numFrames);
        for (size_t i = 0; i < numFrames; ++i) {
            mono[i] = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
        }
    } else {
        mono = std::move(samples);
    }
    wavBuffer_.clear();
    resampleToEngineRate(mono, fileRate, wavBuffer_);
    wavLengthFrames_ = wavBuffer_.size();
    wavPositionFrames_.store(0);
    wavPlaying_.store(false);
    LOGI("WAV loaded: %zu frames at %.0f Hz (from %u Hz)", wavLengthFrames_, sampleRate_, fileRate);
    return true;
}

void AudioEngine::unloadWav() {
    wavPlaying_.store(false);
    wavBuffer_.clear();
    wavLengthFrames_ = 0;
    wavPositionFrames_.store(0);
}

void AudioEngine::wavPlay() {
    if (!wavBuffer_.empty()) {
        wavPlaying_.store(true);
    }
}

void AudioEngine::wavPause() {
    wavPlaying_.store(false);
}

void AudioEngine::wavSeekToFrame(size_t frame) {
    size_t end = wavLengthFrames_;
    if (end > 0 && frame > end) {
        frame = end;
    }
    wavPositionFrames_.store(frame);
}

double AudioEngine::getWavDurationSec() const {
    if (sampleRate_ <= 0.0f || wavLengthFrames_ == 0) return 0.0;
    return static_cast<double>(wavLengthFrames_) / sampleRate_;
}

double AudioEngine::getWavPositionSec() const {
    if (sampleRate_ <= 0.0f) return 0.0;
    return static_cast<double>(wavPositionFrames_.load()) / sampleRate_;
}

bool AudioEngine::isWavPlaying() const {
    return wavPlaying_.load();
}

bool AudioEngine::isWavLoaded() const {
    return !wavBuffer_.empty();
}


void AudioEngine::resampleToEngineRate(const std::vector<float>& src,
                                       uint32_t srcRate,
                                       std::vector<float>& dst) {
    if (src.empty() || sampleRate_ <= 0.0f) {
        dst.clear();
        return;
    }
    if (static_cast<float>(srcRate) == sampleRate_) {
        dst = src;
        return;
    }
    double ratio = sampleRate_ / static_cast<double>(srcRate);
    size_t outFrames = static_cast<size_t>(std::round(src.size() * ratio));
    if (outFrames == 0) {
        dst.clear();
        return;
    }
    dst.resize(outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        double srcIdx = i / ratio;
        size_t j = static_cast<size_t>(srcIdx);
        float frac = static_cast<float>(srcIdx - j);
        if (j + 1 >= src.size()) {
            dst[i] = src[src.size() - 1];
        } else {
            dst[i] = src[j] * (1.0f - frac) + src[j + 1] * frac;
        }
    }
}

oboe::DataCallbackResult AudioEngine::onBothStreamsReady(
    const void* inputData,
    int   numInputFrames,
    void* outputData,
    int   numOutputFrames) {
    const int32_t numFrames = numOutputFrames;
    if (!isRunning_ || numFrames <= 0) {
        return oboe::DataCallbackResult::Continue;
    }

    // Ensure buffers are large enough before touching them
    if (inputBuffer_.size() < static_cast<size_t>(numFrames)) {
        inputBuffer_.resize(numFrames);
    }
    if (outputBufferLeft_.size() < static_cast<size_t>(numFrames)) {
        outputBufferLeft_.resize(numFrames);
        outputBufferRight_.resize(numFrames);
    }

    // Input source: WAV playback or microphone
    const bool useWav = wavPlaying_.load() && !wavBuffer_.empty();
    if (useWav) {
        size_t pos = wavPositionFrames_.load();
        size_t len = wavLengthFrames_;
        size_t toCopy = std::min(static_cast<size_t>(numFrames), len > pos ? len - pos : 0);
        if (toCopy > 0) {
            std::memcpy(inputBuffer_.data(), wavBuffer_.data() + pos, toCopy * sizeof(float));
            wavPositionFrames_.store(pos + toCopy);
        }
        if (toCopy < static_cast<size_t>(numFrames)) {
            std::memset(inputBuffer_.data() + toCopy, 0, (numFrames - toCopy) * sizeof(float));
            wavPlaying_.store(false);
        }
    } else {
        // FullDuplexStream only calls this once input data is available, but the
        // number of frames delivered (numInputFrames) can be smaller than the
        // output block size (e.g. right after start). Copy what we got and
        // silence the remainder so a partial block never repeats stale audio.
        int32_t framesToCopy = std::min(numInputFrames, numFrames);
        if (framesToCopy > 0) {
            std::memcpy(inputBuffer_.data(), inputData, framesToCopy * sizeof(float));
        }
        if (framesToCopy < numFrames) {
            std::memset(inputBuffer_.data() + framesToCopy, 0, (numFrames - framesToCopy) * sizeof(float));
        }
    }

    // Input peak metering and clipping
    float inputPeak = 0.0f;
    bool inputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::fabs(inputBuffer_[i]);
        if (s > inputPeak) inputPeak = s;
        if (s >= kClippingThreshold) inputClip = true;
    }
    inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * kPeakDecay);
    inputPeakLevel_.store(inputPeakHold_);
    if (inputClip) inputClipping_.store(true);

    // Rate-limited debug: input source and level (once per second)
    {
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLog).count() >= 1.0) {
            lastLog = now;
            LOGI("onBothStreamsReady: source=%s numFrames=%d inputPeak=%.4f",
                 useWav ? "WAV" : "mic", numFrames, inputPeak);
        }
    }

    // Set up input pointers (mono guitar input -> stereo)
    inputPtrs_[0] = inputBuffer_.data();
    inputPtrs_[1] = inputBuffer_.data();  // Duplicate mono to stereo

    // Set up output pointers (always process into our buffers for metering)
    float* outData = static_cast<float*>(outputData);
    int32_t numChannels = outputStream_ ? outputStream_->getChannelCount() : 2;
    outputPtrs_[0] = outputBufferLeft_.data();
    outputPtrs_[1] = outputBufferRight_.data();

    // Skip chain processing when bypassed (during preset load) or WAV bypass active
    if (chainBypass_.load() || (useWav && wavBypassChain_.load())) {
        for (int32_t ch = 0; ch < 2; ++ch) {
            std::memcpy(outputPtrs_[ch], inputPtrs_[ch],
                        numFrames * sizeof(float));
        }
    } else {
        // Process through plugin chain (measure CPU time)
        double bufferDurationMs = (numFrames / static_cast<double>(sampleRate_)) * 1000.0;
        auto t0 = std::chrono::high_resolution_clock::now();
        chain_.process(inputPtrs_, outputPtrs_, numFrames);
        auto t1 = std::chrono::high_resolution_clock::now();
        double processMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        float load = static_cast<float>(processMs / bufferDurationMs);
        if (load > 1.0f) load = 1.0f;

        /* Sample subprocess (wine VST) CPU + xruns every ~1s of audio
         * callbacks. The plain cpuLoad_ measurement above misses VST work
         * entirely because the wine subprocess runs in a different
         * address space — getCpuLoad() returns ~1% with a CPU-pinned
         * Helix Native loaded. Sampling rate is human-display rate, not
         * audio rate, so once per second is plenty. */
        constexpr uint32_t kSampleEveryNCallbacks = 100;
        if (++vstSampleCounter_ >= kSampleEveryNCallbacks) {
            vstSampleCounter_ = 0;
            struct timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t nowNs = (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
            uint64_t totalDeltaJiffies = 0;
            int32_t totalUnderruns = 0;
            const size_t n = chain_.getSize();
            for (size_t i = 0; i < n; ++i) {
                guitarrackcraft::IPlugin* p = chain_.getPlugin((int)i);
                if (!p) continue;
                totalUnderruns += p->getUnderrunCount();
                int pid = p->getSubprocessPid();
                if (pid <= 0) continue;
                /* /proc/<pid>/stat fields utime (14) + stime (15) are
                 * cumulative jiffies (typically 100Hz on Android) since
                 * subprocess start. Read both, sum, diff against last
                 * sample to derive jiffies-per-wall-second. */
                char path[64];
                std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
                FILE* f = std::fopen(path, "r");
                if (!f) continue;
                char buf[512];
                size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                if (got == 0) continue;
                buf[got] = 0;
                /* Skip past "comm" (in parens — name can contain spaces),
                 * then we're at field 3 (state). utime is field 14,
                 * stime is field 15. */
                char* p2 = std::strrchr(buf, ')');
                if (!p2) continue;
                unsigned long long fields[16] = {0};
                int read = std::sscanf(p2 + 1,
                    " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                    &fields[13], &fields[14]);
                if (read != 2) continue;
                uint64_t cur = fields[13] + fields[14];
                auto it = vstLastJiffies_.find(pid);
                if (it != vstLastJiffies_.end()) {
                    totalDeltaJiffies += cur - it->second;
                    it->second = cur;
                } else {
                    vstLastJiffies_[pid] = cur;
                }
            }
            if (vstLastSampleNs_ > 0 && nowNs > vstLastSampleNs_) {
                uint64_t elapsedNs = nowNs - vstLastSampleNs_;
                /* sysconf(_SC_CLK_TCK) is the jiffies-per-second on this
                 * system; on Android it's 100. Compute fraction of one
                 * CPU's wall time the subprocesses used in aggregate. */
                long clk = sysconf(_SC_CLK_TCK);
                if (clk <= 0) clk = 100;
                double secs = (double)elapsedNs / 1.0e9;
                double frac = ((double)totalDeltaJiffies / (double)clk) / secs;
                if (frac > 1.0) frac = 1.0;
                vstCpuLoad_.store((float)frac);
            }
            vstLastSampleNs_ = nowNs;
            vstUnderruns_.store(totalUnderruns);
        }
        cpuLoad_.store(load);
    }

    // Output peak metering (from buffers we wrote to)
    float outputPeak = 0.0f;
    bool outputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::max(std::fabs(outputBufferLeft_[i]), std::fabs(outputBufferRight_[i]));
        if (s > outputPeak) outputPeak = s;
        if (s >= kClippingThreshold) outputClip = true;
    }
    outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
    outputPeakLevel_.store(outputPeakHold_);
    if (outputClip) outputClipping_.store(true);

    // Feed recorder (lock-free ring buffer write)
    if (recorder_.isRecording()) {
        recorder_.feedAudio(inputBuffer_.data(),
                            outputBufferLeft_.data(),
                            outputBufferRight_.data(),
                            numFrames);
    }

    // Rate-limited debug: output level (once per second)
    {
        static auto lastLogOut = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogOut).count() >= 1.0) {
            lastLogOut = now;
            LOGI("onBothStreamsReady: outputPeak=%.4f", outputPeak);
        }
    }

    // Copy to output (stereo: deinterleave; mono: mix)
    if (numChannels == 2) {
        for (int32_t i = 0; i < numFrames; ++i) {
            outData[i * 2] = outputBufferLeft_[i];
            outData[i * 2 + 1] = outputBufferRight_[i];
        }
    } else {
        for (int32_t i = 0; i < numFrames; ++i) {
            outData[i] = (outputBufferLeft_[i] + outputBufferRight_[i]) * 0.5f;
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorBeforeClose tid=%ld stream=%p error=%s (set isRunning_=false so callback bails)",
         getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    // Signal callback to exit immediately; Oboe will close the stream after we return.
    isRunning_ = false;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorAfterClose tid=%ld stream=%p error=%s", getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    isRunning_ = false;
    // Do NOT reset() the stream here. The underlying AAudio stream is already closed by
    // Oboe/the system (e.g. AudioBoost cancelling boost can trigger this). If we run
    // outputStream_.reset() on this thread, the destructor (~AAudioLoader) runs while
    // the audio callback thread may still be inside Oboe -> pthread_mutex_lock on
    // destroyed mutex (SIGABRT). Leave the stream object alive; stop() -> closeStreams()
    // will run later (from lifecycle or user) and destroy it on the main thread after
    // the 250ms sleep, when the callback thread is guaranteed idle.
}

bool AudioEngine::createAudioStreams(float sampleRate) {
    // Enable MMAP data path for lowest latency (must be set before opening streams).
    // Without this, AAudio uses the legacy non-MMAP path and cannot grant exclusive mode.
    oboe::OboeExtensions::setMMapEnabled(true);
    LOGI("MMAP supported=%d enabled=%d", oboe::OboeExtensions::isMMapSupported(),
         oboe::OboeExtensions::isMMapEnabled());

    // --- Input stream (mono, for guitar) ---
    // Force AAudio API — OpenSL ES cannot do exclusive or MMAP.
    // Oboe's QuirksManager may silently choose OpenSL ES otherwise.
    // Do NOT set a callback on input — only the output stream drives the callback.
    // The input is read inside the output callback, synchronized via FullDuplexStream.

    // 打开输入流：优先"现场演奏"预置（无 AGC/NS 语音处理，延迟最低、不破坏吉他信号）。
    // VoicePerformance 需要 API 30+，老设备回退 Generic。注意不要用 VoiceRecognition：
    // 它会开启自动增益/降噪等语音处理，既增加延迟也会改变吉他音色。
    // 采样率回退：请求的采样率（默认 48000）不被设备支持时（USB 声卡如 Apogee Jam 96k
    // 只支持 44.1/96kHz），依次尝试 44100/48000，最后回退系统默认设备。
    // 每次新建 builder，避免依赖已设置过的 deviceId 状态。
    oboe::AudioStream* inputStreamPtr = nullptr;
    auto openInput = [&](int32_t deviceId, oboe::AudioApi api, int32_t rate,
                         oboe::InputPreset preset) -> oboe::Result {
        oboe::AudioStreamBuilder b;
        b.setDirection(oboe::Direction::Input)
         ->setAudioApi(api)
         ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
         ->setSharingMode(oboe::SharingMode::Exclusive)
         ->setFormat(oboe::AudioFormat::Float)
         ->setChannelCount(1)
         ->setSampleRate(rate)
         ->setInputPreset(preset);
        if (deviceId != 0) {
            b.setDeviceId(deviceId);
            LOGI("Input device ID set to %d", deviceId);
        }
        return b.openStream(&inputStreamPtr);
    };

    const int32_t requestedRate = static_cast<int32_t>(sampleRate);
    oboe::Result result = openInput(inputDeviceId_, oboe::AudioApi::AAudio,
                                    requestedRate, oboe::InputPreset::VoicePerformance);
    if (result != oboe::Result::OK) {
        // VoicePerformance 需要 API 30+；老设备用 Generic（同样无语音处理）
        LOGI("Input open with VoicePerformance failed (%s), retrying with Generic preset",
             oboe::convertToText(result));
        result = openInput(inputDeviceId_, oboe::AudioApi::AAudio,
                           requestedRate, oboe::InputPreset::Generic);
    }
    // USB 声卡可能不支持 48kHz（如 Apogee Jam 96k 只支持 44.1/96kHz）→ 试 44.1k
    if (result != oboe::Result::OK && requestedRate != 44100) {
        LOGW("Input open at %d Hz failed (%s); retrying at 44100 Hz",
             requestedRate, oboe::convertToText(result));
        result = openInput(inputDeviceId_, oboe::AudioApi::Unspecified,
                           44100, oboe::InputPreset::Generic);
    }
    // 再试 48k（可能上一步 44.1k 反而不支持）
    if (result != oboe::Result::OK && requestedRate != 48000) {
        LOGW("Input open at 44100 Hz failed (%s); retrying at 48000 Hz",
             oboe::convertToText(result));
        result = openInput(inputDeviceId_, oboe::AudioApi::Unspecified,
                           48000, oboe::InputPreset::Generic);
    }
    if (result != oboe::Result::OK) {
        // 用户保存的设备可能已不可用（如 USB 声卡被拔出/耳机麦克风移除），
        // 回退到系统默认输入设备，避免引擎因此完全无法启动。
        LOGW("Input open with device %d failed (%s); retrying with default device",
             inputDeviceId_, oboe::convertToText(result));
        inputDeviceId_ = 0;
        result = openInput(0, oboe::AudioApi::Unspecified, requestedRate,
                           oboe::InputPreset::Generic);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open input stream: %s", oboe::convertToText(result));
            return false;
        }
    }
    inputStream_.reset(inputStreamPtr);

    LOGI("Input stream opened: rate=%d api=%d sharing=%d perf=%d mmap=%d preset=%d",
         inputStream_->getSampleRate(),
         static_cast<int>(inputStream_->getAudioApi()),
         static_cast<int>(inputStream_->getSharingMode()),
         static_cast<int>(inputStream_->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(inputStream_.get()),
         static_cast<int>(inputStream_->getInputPreset()));

    // Use actual sample rate from stream
    sampleRate_ = static_cast<float>(inputStream_->getSampleRate());

    // --- Output stream (stereo) ---
    oboe::AudioStreamBuilder outputBuilder;
    outputBuilder.setDirection(oboe::Direction::Output)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(2)
           ->setSampleRate(static_cast<int32_t>(sampleRate_))
           // Note: "VoicePerformance" exists only as an INPUT preset in AAudio
           // (already used for the input stream above); there is no such output
           // usage. Game is the correct low-latency output usage.
           ->setUsage(oboe::Usage::Game)
           // Allocate an audio session so the Kotlin layer can disable OEM
           // post-processing effects (Dolby/MiSound) that get inserted on it
           // and audibly distort a live guitar signal.
           ->setSessionId(oboe::SessionId::Allocate)
           ->setDataCallback(this)
           ->setErrorCallback(this);

    if (outputDeviceId_ != 0) {
        outputBuilder.setDeviceId(outputDeviceId_);
        LOGI("Output device ID set to %d", outputDeviceId_);
    }

    oboe::AudioStream* outputStreamPtr = nullptr;
    result = outputBuilder.openStream(&outputStreamPtr);
    if (result != oboe::Result::OK) {
        // AAudio failed — retry without forcing API
        LOGE("AAudio output open failed (%s), retrying with default API", oboe::convertToText(result));
        outputBuilder.setAudioApi(oboe::AudioApi::Unspecified);
        result = outputBuilder.openStream(&outputStreamPtr);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output stream: %s", oboe::convertToText(result));
            closeStreams();
            return false;
        }
    }

    LOGI("Output stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(outputStreamPtr->getAudioApi()),
         static_cast<int>(outputStreamPtr->getSharingMode()),
         static_cast<int>(outputStreamPtr->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(outputStreamPtr));

    // Determine callback block size.
    // If the user requested a specific buffer size, use it directly.
    // Otherwise use the hardware burst size as the callback block: processing
    // at the burst size lets the stream buffers be shrunk down to the burst
    // (lowest latency). LV2 convolver plugins already round the block size up
    // to a power of 2 internally (LV2Plugin::activate), so we don't need a
    // power-of-2 framesPerCallback here — forcing one only inflates the
    // minimum buffer and adds latency.
    {
        if (requestedBufferFrames_ > 0) {
            callbackFrameCount_ = static_cast<uint32_t>(requestedBufferFrames_);
            LOGI("Using user-requested buffer frames: %u", callbackFrameCount_);
            outputStreamPtr->close();
            delete outputStreamPtr;
            outputStreamPtr = nullptr;

            outputBuilder.setFramesPerCallback(requestedBufferFrames_);
            result = outputBuilder.openStream(&outputStreamPtr);
            if (result != oboe::Result::OK) {
                LOGE("Failed to reopen output stream with requested buffer: %s",
                     oboe::convertToText(result));
                closeStreams();
                return false;
            }
        } else {
            int32_t framesPerBurst = outputStreamPtr->getFramesPerBurst();
            callbackFrameCount_ = (framesPerBurst > 0)
                                      ? static_cast<uint32_t>(framesPerBurst)
                                      : 128u;
            LOGI("Using native burst as callback block size: %u frames",
                 callbackFrameCount_);
        }
    }

    outputStream_.reset(outputStreamPtr);

    // Full-duplex 同步：把输入/输出流交给 FullDuplexStream 对齐。启动时会先排空
    // 输入缓冲并维持输入/输出均衡，回调内只在实际有数据时才读取，避免读到陈旧
    // 数据（USB 声卡场景下陈旧数据会累积成几十~上百毫秒的恒定延迟）。
    setInputStream(inputStream_.get());
    setOutputStream(outputStream_.get());

    // 最小化应用侧缓冲以降低延迟（USB 声卡/外置接口影响最大）：
    // Oboe 打开流时默认分配的缓冲通常是硬件 burst 的 2-4 倍，音频会因此在
    // 应用的输入/输出队列里多停留若干毫秒。显式收缩到设备允许的最小值，
    // 下限为 max(burst, 回调块大小)，保证每次回调都能读满/写满一块。
    // 注意：不要用 setFramesPerCallback 强制大于 burst 的回调块，那会把最小
    // 缓冲顶到回调块大小，反而增加延迟（见上面的 callbackFrameCount_ 逻辑）。
    {
        int32_t outBurst = outputStream_->getFramesPerBurst();
        int32_t minOut = std::max(outBurst, static_cast<int32_t>(callbackFrameCount_));
        if (minOut > 0) {
            auto shrink = outputStream_->setBufferSizeInFrames(minOut);
            if (shrink == oboe::Result::OK) {
                LOGI("Output buffer shrunk to %d frames (burst=%d, callback=%u)",
                     shrink.value(), outBurst, callbackFrameCount_);
            } else {
                LOGW("Failed to shrink output buffer to %d: %s",
                     minOut, oboe::convertToText(shrink.error()));
            }
        }

        if (inputStream_) {
            int32_t inBurst = inputStream_->getFramesPerBurst();
            int32_t minIn = std::max(inBurst, static_cast<int32_t>(callbackFrameCount_));
            if (minIn > 0) {
                auto shrinkIn = inputStream_->setBufferSizeInFrames(minIn);
                if (shrinkIn == oboe::Result::OK) {
                    LOGI("Input buffer shrunk to %d frames (burst=%d, callback=%u)",
                         shrinkIn.value(), inBurst, callbackFrameCount_);
                } else {
                    LOGW("Failed to shrink input buffer to %d: %s",
                         minIn, oboe::convertToText(shrinkIn.error()));
                }
            }
        }
    }

    // Start both streams in sync (input first, then output). 显式限定基类作用域：
    // 本类自己的 start(float,...) 有全默认参数，直接调用会递归。
    result = oboe::FullDuplexStream::start();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start duplex streams: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    // 输入/输出采样率一致性检查：若两者不同，框架会静默重采样，既增加延迟
    // 又可能破坏音色。正常情况（USB 声卡 44.1/48k 都能协商）应一致。
    {
        int32_t inRate = inputStream_->getSampleRate();
        int32_t outRate = outputStream_->getSampleRate();
        if (inRate != outRate) {
            LOGW("SAMPLE RATE MISMATCH: input=%d Hz vs output=%d Hz -> framework "
                 "resampling adds latency. Consider forcing both to the same rate.",
                 inRate, outRate);
        }
    }

    // 流启动后再收缩一次：部分设备（尤其 USB 声卡）只有在流运行起来后才真正
    // 应用 setBufferSizeInFrames。二次请求 burst 大小的缓冲，把应用侧停留时间
    // 压到最低。
    {
        int32_t outBurst = outputStream_->getFramesPerBurst();
        if (outBurst > 0 && outputStream_->getBufferSizeInFrames() > outBurst) {
            auto r = outputStream_->setBufferSizeInFrames(outBurst);
            if (r == oboe::Result::OK) {
                LOGI("Post-start output shrink to %d frames (burst=%d)", r.value(), outBurst);
            } else {
                LOGW("Post-start output shrink to %d failed: %s",
                     outBurst, oboe::convertToText(r.error()));
            }
        }
        if (inputStream_) {
            int32_t inBurst = inputStream_->getFramesPerBurst();
            if (inBurst > 0 && inputStream_->getBufferSizeInFrames() > inBurst) {
                auto r = inputStream_->setBufferSizeInFrames(inBurst);
                if (r == oboe::Result::OK) {
                    LOGI("Post-start input shrink to %d frames (burst=%d)", r.value(), inBurst);
                } else {
                    LOGW("Post-start input shrink to %d failed: %s",
                         inBurst, oboe::convertToText(r.error()));
                }
            }
        }
    }

    // 后台延迟调谐器（参考 Amp Rack 的 LatencyTuner 策略）：部分设备在流刚
    // 启动时会拒绝收缩输入缓冲，或在系统策略变化后（如切后台再回前台）才
    // 允许更小的值。定期重试收缩，并在出现新的 xrun 时自动回退一个 burst，
    // 在"尽量小的缓冲"和"不丢音"之间自适应。
    latencyTunerRunning_.store(true);
    tunerXrunsAtShrink_.store(-1);
    latencyTunerThread_ = std::thread(&AudioEngine::latencyTunerLoop, this);

    // Allocate buffers
    int32_t bufferSize = outputStream_->getBufferSizeInFrames();
    inputBuffer_.resize(bufferSize);
    outputBufferLeft_.resize(bufferSize);
    outputBufferRight_.resize(bufferSize);

    // 详细诊断日志：用于核对 USB 声卡场景下实际协商出的采样率/缓冲/共享模式。
    LOGI("AUDIO STREAM CFG: rate=%d callbackFrames=%u "
         "in{api=%d sharing=%d perf=%d mmap=%d burst=%d buf=%d cap=%d} "
         "out{api=%d sharing=%d perf=%d mmap=%d burst=%d buf=%d cap=%d}",
         static_cast<int>(sampleRate_), callbackFrameCount_,
         static_cast<int>(inputStream_->getAudioApi()),
         static_cast<int>(inputStream_->getSharingMode()),
         static_cast<int>(inputStream_->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(inputStream_.get()),
         inputStream_->getFramesPerBurst(),
         inputStream_->getBufferSizeInFrames(),
         inputStream_->getBufferCapacityInFrames(),
         static_cast<int>(outputStream_->getAudioApi()),
         static_cast<int>(outputStream_->getSharingMode()),
         static_cast<int>(outputStream_->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(outputStream_.get()),
         outputStream_->getFramesPerBurst(),
         outputStream_->getBufferSizeInFrames(),
         outputStream_->getBufferCapacityInFrames());
    LOGI("Audio streams created: %d Hz, buffer size: %d frames",
         static_cast<int>(sampleRate_), bufferSize);

    return true;
}

void AudioEngine::latencyTunerLoop() {
    // Sleep helper: abortable in 250ms steps so engine teardown never waits
    // more than a quarter second for this thread.
    auto nap = [this](int totalMs) {
        int slept = 0;
        while (slept < totalMs && latencyTunerRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            slept += 250;
        }
    };

    // Give the streams a moment to settle before the first tuning attempt.
    nap(2000);

    int attempts = 0;
    while (latencyTunerRunning_.load() && attempts < 20) {
        attempts++;
        auto* in = inputStream_.get();
        if (!in) break;

        int32_t inBurst = in->getFramesPerBurst();
        int32_t target = std::max(inBurst, static_cast<int32_t>(callbackFrameCount_));
        int32_t current = in->getBufferSizeInFrames();
        int32_t xruns = getXRunCount();

        // Back off when the last successful shrink was followed by new xruns:
        // the buffer went below what the capture path can reliably deliver.
        if (tunerXrunsAtShrink_.load() >= 0 && xruns > tunerXrunsAtShrink_.load()) {
            int32_t relaxed = current + inBurst;
            auto r = in->setBufferSizeInFrames(relaxed);
            LOGW("LatencyTuner: xruns after shrink (%d -> %d), relaxing input buffer to %d frames",
                 tunerXrunsAtShrink_.load(), xruns,
                 r == oboe::Result::OK ? r.value() : relaxed);
            tunerXrunsAtShrink_.store(-1);
            nap(3000);
            continue;
        }

        if (current > target && target > 0) {
            // Retry the shrink — some devices only allow it after the stream
            // has been running for a while, or clamp to intermediate sizes.
            auto r = in->setBufferSizeInFrames(target);
            if (r == oboe::Result::OK && r.value() < current) {
                LOGI("LatencyTuner: input buffer %d -> %d frames (target=%d)",
                     current, r.value(), target);
                tunerXrunsAtShrink_.store(xruns);
                nap(2000);
                continue;
            }
            if (r == oboe::Result::OK && r.value() >= current) {
                // Accepted but clamped by the audio policy — the requested
                // size is below the enforced minimum. Keep retrying a few
                // times in case the policy relaxes, but log it so the
                // limitation is visible from logcat.
                LOGW("LatencyTuner: input shrink to %d clamped at %d frames "
                     "(system-enforced minimum)", target, r.value());
            }
        } else if (current <= target) {
            // Fully shrunk. If the last shrink stayed xrun-free across the
            // observation window, tuning is done; otherwise the backoff
            // branch above will have relaxed the buffer already.
            LOGI("LatencyTuner: input buffer at target %d frames, tuning done", target);
            break;
        }
        nap(1000);
    }
}

void AudioEngine::closeStreams() {
    LOGI("closeStreams() ENTER tid=%ld (caller thread; AudioTrack callback is different tid)", getTid());

    // Stop the latency tuner first — it touches the streams from its own thread.
    latencyTunerRunning_.store(false);
    if (latencyTunerThread_.joinable()) {
        latencyTunerThread_.join();
    }

    // First, signal the callback to stop immediately to prevent new callbacks
    // from starting while we're tearing down.
    isRunning_.store(false);
    
    if (inputStream_) {
        LOGI("closeStreams() input stream stop+close+reset");
        inputStream_->stop();
        inputStream_->close();
        inputStream_.reset();
    }

    if (outputStream_) {
        LOGI("closeStreams() output stream stop");
        outputStream_->stop();
        LOGI("closeStreams() output stream close");
        outputStream_->close();
        // Wait for any in-flight AAudio callback to finish before destroying the
        // stream object. close() already removed us from AAudioStreamCollection,
        // so late callbacks will see !isStreamAlive and return Stop. If we reset()
        // too soon, ~AudioStreamAAudio / ~AAudioLoader run while the AudioTrack
        // thread is still in getStream() -> destroyed mutex (SIGABRT).
        // 
        // INCREASED from 250ms to 500ms: The 250ms delay was not sufficient on some
        // devices (e.g., OnePlus) where the audio callback thread takes longer to
        // fully exit, especially when the app is being force-closed or when there
        // are concurrent EGL/GL operations that may interfere with the audio subsystem.
        static constexpr int kStreamCloseSleepMs = 500;
        LOGI("closeStreams() sleep %dms before outputStream_.reset() [tid=%ld]", kStreamCloseSleepMs, getTid());
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamCloseSleepMs));
        LOGI("closeStreams() outputStream_.reset() NOW tid=%ld", getTid());
        outputStream_.reset();
        LOGI("closeStreams() output stream destroyed tid=%ld", getTid());
    }
    LOGI("closeStreams() done");
}

} // namespace guitarrackcraft

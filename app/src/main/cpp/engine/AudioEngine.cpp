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

int32_t AudioEngine::isMMapUsedSafe(oboe::AudioStream* stream) {
    if (!stream || stream->getAudioApi() != oboe::AudioApi::AAudio) {
        return 0;
    }
    return oboe::OboeExtensions::isMMapUsed(stream);
}

bool AudioEngine::start(float sampleRate, int32_t inputDeviceId,
                        int32_t outputDeviceId, int32_t bufferBursts,
                        int32_t inputCushionMs) {
    LOGI("start() ENTER tid=%ld sampleRate=%.0f inputDev=%d outputDev=%d bursts=%d cushionMs=%d isRunning_=%d",
         getTid(), sampleRate, inputDeviceId, outputDeviceId, bufferBursts, inputCushionMs,
         isRunning_ ? 1 : 0);
    if (isRunning_) {
        LOGI("start() early return (already running)");
        return true;
    }

    sampleRate_ = sampleRate;
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    requestedBufferBursts_ = (bufferBursts >= 1) ? bufferBursts : 4;
    inputCushionFrames_ = static_cast<int32_t>(inputCushionMs * 0.001f * sampleRate);
    if (inputCushionFrames_ < 0) inputCushionFrames_ = 0;
    // Never let the cushion eat more than a quarter of the input ring, otherwise
    // real input would be dropped instead of absorbed.
    const int32_t maxCushion = static_cast<int32_t>(inputRing_.capacityFrames() / 4);
    if (inputCushionFrames_ > maxCushion) inputCushionFrames_ = maxCushion;

    if (!createAudioStreams(sampleRate)) {
        LOGE("Failed to create audio streams");
        return false;
    }

    // Reset and pre-fill the decoupling rings. ringTargetFrames_ is the
    // constant output-side latency the DSP path adds; pre-filling it with
    // silence means the callback can keep draining the output ring while the
    // first input burst is still making its way through the HAL, so the ring
    // can never underflow (which would sound like a gap/buzz) right after
    // start. The fill is aligned to whole callback blocks.
    {
        int32_t cb = static_cast<int32_t>(callbackFrameCount_);
        ringTargetFrames_ = static_cast<int32_t>(kRingTargetMs * 0.001f * sampleRate);
        if (cb > 0) {
            ringTargetFrames_ = (ringTargetFrames_ / cb) * cb;
            if (ringTargetFrames_ < cb) ringTargetFrames_ = cb;
        }
        inputRing_.reset();
        outputRing_.reset();
        // Input cushion: a fixed input-side delay that decouples the DSP from
        // HAL scheduling jitter. Same mechanism as the output pre-fill above.
        if (inputCushionFrames_ > 0) {
            inputRing_.writeSilence(static_cast<size_t>(inputCushionFrames_));
            LOGI("Input cushion armed: %d frames (%.1f ms)", inputCushionFrames_,
                 (inputCushionFrames_ / sampleRate_) * 1000.0);
        }
        // outputRing_ stores interleaved stereo: 2 floats per frame
        outputRing_.writeSilence(static_cast<size_t>(ringTargetFrames_) * 2);
        LOGI("Ring buffers armed: target=%d frames (%.1f ms)", ringTargetFrames_,
             (ringTargetFrames_ / sampleRate_) * 1000.0);
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
        info.outputMMap = isMMapUsedSafe(outputStream_.get());
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

    // The DSP path runs decoupled through outputRing_: its pre-filled fill
    // level (ringTargetFrames_) IS the constant latency the plugin chain adds,
    // regardless of how irregular the HAL input supply is. On top of that sits
    // the output stream's buffer fill, which on non-MMAP paths can be a few
    // bursts (on policy-capped deep-buffer paths the burst equals the buffer
    // itself, so there we report the buffer alone to avoid triple-counting).
    if (ringTargetFrames_ > 0) {
        latencyMs += (ringTargetFrames_ / sampleRate_) * 1000.0;
    }
    // Input cushion sits in front of the DSP, so it adds to the total.
    if (inputCushionFrames_ > 0) {
        latencyMs += (inputCushionFrames_ / sampleRate_) * 1000.0;
    }
    if (outputStream_) {
        int32_t bufferSize = outputStream_->getBufferSizeInFrames();
        int32_t burst = outputStream_->getFramesPerBurst();
        int32_t marginFrames = bufferSize;
        if (burst > 0 && burst < bufferSize / 2) {
            marginFrames += burst * 2;
        }
        latencyMs += (marginFrames / sampleRate_) * 1000.0;
    }

    return latencyMs;
}

int32_t AudioEngine::getFramesPerBurst() const {
    return outputStream_ ? outputStream_->getFramesPerBurst() : 0;
}

int32_t AudioEngine::getOutputBufferFrames() const {
    return outputStream_ ? outputStream_->getBufferSizeInFrames() : 0;
}

int32_t AudioEngine::getOutputBufferCapacityFrames() const {
    return outputStream_ ? outputStream_->getBufferCapacityInFrames() : 0;
}

int32_t AudioEngine::getInputBufferFrames() const {
    return inputStream_ ? inputStream_->getBufferSizeInFrames() : 0;
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

// --- Pre-chain input gain + noise gate ---

void AudioEngine::setPreGainDb(float db) {
    if (db < -24.0f) db = -24.0f;
    if (db > 24.0f) db = 24.0f;
    preGainDb_.store(db);
}

void AudioEngine::setGateThresholdDb(float db) {
    if (db < -120.0f) db = -120.0f;
    if (db > -6.0f) db = -6.0f;
    gateThresholdDb_.store(db);
}

void AudioEngine::setGateHysteresisDb(float db) {
    if (db < 0.0f) db = 0.0f;
    if (db > 12.0f) db = 12.0f;
    gateHysteresisDb_.store(db);
}

void AudioEngine::setGateFloorDb(float db) {
    if (db < -100.0f) db = -100.0f;
    if (db > -20.0f) db = -20.0f;
    gateFloorDb_.store(db);
}

void AudioEngine::setGateAttackMs(float ms) {
    if (ms < 0.5f) ms = 0.5f;
    if (ms > 50.0f) ms = 50.0f;
    gateAttackMs_.store(ms);
}

void AudioEngine::setGateHoldMs(float ms) {
    if (ms < 0.0f) ms = 0.0f;
    if (ms > 500.0f) ms = 500.0f;
    gateHoldMs_.store(ms);
}

void AudioEngine::setGateReleaseMs(float ms) {
    if (ms < 10.0f) ms = 10.0f;
    if (ms > 1000.0f) ms = 1000.0f;
    gateReleaseMs_.store(ms);
}

void AudioEngine::setOutputGainDb(float db) {
    if (db < -24.0f) db = -24.0f;
    if (db > 24.0f) db = 24.0f;
    outputGainDb_.store(db);
}

/**
 * Apply input pre-gain and a simple downward-expander noise gate to the
 * input buffer, in place, before metering and the plugin chain.
 *
 * Gate: peak envelope follower; when the envelope falls below the
 * threshold the gain eases toward 0 over the release time (soft close),
 * and jumps back to 1 quickly when signal returns (fast attack, no click
 * thanks to the smoothed gain ramp). Runs entirely on the audio thread
 * with only atomic reads for parameters — no locks, no allocation.
 */
void AudioEngine::applyPreGainAndGate(float* buf, int32_t numFrames) {
    // Refresh cached linear gain if the dB setting changed
    float db = preGainDb_.load(std::memory_order_relaxed);
    if (db != preGainDbCached_) {
        preGainDbCached_ = db;
        preGainLin_ = std::pow(10.0f, db / 20.0f);
    }

    const float thresholdDb = gateThresholdDb_.load(std::memory_order_relaxed);
    const bool gateEnabled = thresholdDb > -96.0f;

    if (!gateEnabled) {
        // Gain only
        for (int32_t i = 0; i < numFrames; ++i) {
            buf[i] *= preGainLin_;
        }
        return;
    }

    const float hysteresisDb = gateHysteresisDb_.load(std::memory_order_relaxed);
    const float floorDb = gateFloorDb_.load(std::memory_order_relaxed);
    const float attackMs = std::max(0.5f, gateAttackMs_.load(std::memory_order_relaxed));
    const float holdMs = std::max(0.0f, gateHoldMs_.load(std::memory_order_relaxed));
    const float releaseMs = std::max(1.0f, gateReleaseMs_.load(std::memory_order_relaxed));

    const float thresholdLin = std::pow(10.0f, thresholdDb / 20.0f);
    const float openLin = thresholdLin * std::pow(10.0f, hysteresisDb / 20.0f);
    // Closing settles at the floor rather than silence: a hard cut to 0 chops
    // decaying notes, while -80 dB is inaudible on its own.
    const float floorLin = std::pow(10.0f, floorDb / 20.0f);
    const int32_t holdFrames = static_cast<int32_t>(holdMs * 0.001f * sampleRate_);
    // Envelope decay per sample: ~50 ms to fall to half
    const float envDecay = std::pow(0.5f, 1.0f / (0.05f * sampleRate_));
    // Envelope attack: rises over attackMs (default ~5 ms). The K80 capture
    // stream emits periodic 1-2ms noise spikes; a peak-following (instant-attack)
    // envelope latches onto every spike, keeping the gate open and letting the
    // NAM amplify the spikes into constant buzz even with the guitar volume off.
    // A slow attack ignores sub-5ms spikes while pick transients (10-50ms) still
    // open the gate normally.
    const float envAttack = 1.0f - std::pow(0.001f, 1.0f / (attackMs * 0.001f * sampleRate_));
    // Gain ramp coefficients: open ~2 ms, close over the release time
    const float openCoef = 1.0f - std::pow(0.001f, 1.0f / (0.002f * sampleRate_));
    const float closeCoef = 1.0f - std::pow(0.001f, 1.0f / (releaseMs * 0.001f * sampleRate_));

    for (int32_t i = 0; i < numFrames; ++i) {
        float s = buf[i];
        // Envelope follower on the pre-gain signal so the threshold stays
        // meaningful regardless of the gain setting. Slow attack rejects the
        // input stream's periodic 1-2ms spikes (see above).
        float a = std::fabs(s);
        if (a > gateEnv_) {
            gateEnv_ += (a - gateEnv_) * envAttack;
        } else {
            gateEnv_ *= envDecay;
        }

        // Hysteresis + hold state machine: open immediately above the open
        // threshold; hold open while above the close threshold; only close
        // after the envelope has stayed below the close threshold for a full
        // hold window (so decaying notes aren't chopped, and the gate can't
        // flap on brief level dips).
        if (gateEnv_ >= openLin) {
            gateOpen_ = true;
            gateHoldFrames_ = 0;
        } else if (gateEnv_ >= thresholdLin) {
            gateHoldFrames_ = 0;  // still above close threshold, keep open
        } else if (gateHoldFrames_ < holdFrames) {
            ++gateHoldFrames_;
        } else {
            gateOpen_ = false;
        }

        float target = gateOpen_ ? 1.0f : floorLin;
        float coef = (target > gateGain_) ? openCoef : closeCoef;
        gateGain_ += coef * (target - gateGain_);

        buf[i] = s * gateGain_ * preGainLin_;
    }
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

    const int32_t block = static_cast<int32_t>(callbackFrameCount_);
    if (block <= 0) {
        return oboe::DataCallbackResult::Continue;
    }

    // Ensure DSP block buffers are large enough before touching them
    if (processIn_.size() < static_cast<size_t>(block)) {
        processIn_.resize(block);
    }
    if (processOutLeft_.size() < static_cast<size_t>(block)) {
        processOutLeft_.resize(block);
        processOutRight_.resize(block);
    }

    // ---- Input side: push whatever the HAL delivered into the ring ----
    // WAV playback or microphone. No block-size coupling: irregular input
    // arrival (e.g. the K80's 10-13ms HAL supply period) is absorbed here and
    // never starves the DSP, because the chain consumes from the ring on its
    // own fixed cadence below.
    const bool useWav = wavPlaying_.load() && !wavBuffer_.empty();
    if (useWav) {
        size_t pos = wavPositionFrames_.load();
        size_t len = wavLengthFrames_;
        size_t toCopy = std::min(static_cast<size_t>(numFrames), len > pos ? len - pos : 0);
        if (toCopy > 0) {
            inputRing_.write(wavBuffer_.data() + pos, toCopy);
            wavPositionFrames_.store(pos + toCopy);
        }
        if (toCopy < static_cast<size_t>(numFrames)) {
            inputRing_.writeSilence(numFrames - toCopy);
            wavPlaying_.store(false);
        }
    } else if (numInputFrames > 0) {
        const size_t n = static_cast<size_t>(numInputFrames);
        if (inputRing_.writable() >= n) {
            inputRing_.write(static_cast<const float*>(inputData), n);
        } else {
            // Ring full: the DSP is not draining fast enough. Drop the burst and
            // count it so the diagnostics panel can surface the condition.
            inputRingOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ---- DSP: consume fixed callback-sized blocks whenever buffered ----
    while (inputRing_.readable() >= static_cast<size_t>(block)) {
        inputRing_.read(processIn_.data(), block);

        // Feed the tuner with raw input: pitch is level-independent, and the
        // gate below would mute quiet notes before they could be measured.
        tuner_.pushSamples(processIn_.data(), block);

        // Pre-chain input gain + noise gate. Applied before metering so the
        // input meter reflects what actually hits the plugin chain.
        applyPreGainAndGate(processIn_.data(), block);

        // Input peak metering and clipping
        float inputPeak = 0.0f;
        bool inputClip = false;
        for (int32_t i = 0; i < block; ++i) {
            float s = std::fabs(processIn_[i]);
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
                LOGI("onBothStreamsReady: source=%s block=%d inputPeak=%.4f",
                     useWav ? "WAV" : "mic", block, inputPeak);
            }
        }

        // Set up input pointers (mono guitar input -> stereo)
        inputPtrs_[0] = processIn_.data();
        inputPtrs_[1] = processIn_.data();  // Duplicate mono to stereo

        // Set up output pointers (process into our buffers for metering)
        outputPtrs_[0] = processOutLeft_.data();
        outputPtrs_[1] = processOutRight_.data();

        // Skip chain processing when bypassed (during preset load) or WAV bypass active
        if (chainBypass_.load() || (useWav && wavBypassChain_.load())) {
            for (int32_t ch = 0; ch < 2; ++ch) {
                std::memcpy(outputPtrs_[ch], inputPtrs_[ch],
                            block * sizeof(float));
            }
        } else {
            // Process through plugin chain (measure CPU time)
            double bufferDurationMs = (block / static_cast<double>(sampleRate_)) * 1000.0;
            auto t0 = std::chrono::high_resolution_clock::now();
            chain_.process(inputPtrs_, outputPtrs_, block);
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

        // Post-chain output gain (master volume). Refreshed from the atomic on
        // change; applied to both channels after the chain so users can boost or
        // trim the final level. Runs before the limiter so the limiter still
        // catches overs produced by the boost.
        float outDb = outputGainDb_.load(std::memory_order_relaxed);
        if (outDb != outputGainDbCached_) {
            outputGainDbCached_ = outDb;
            outputGainLin_ = std::pow(10.0f, outDb / 20.0f);
        }
        if (outputGainLin_ != 1.0f) {
            for (int32_t i = 0; i < block; ++i) {
                processOutLeft_[i] *= outputGainLin_;
                processOutRight_[i] *= outputGainLin_;
            }
        }

        // Output soft limiter: hot amp models (NAM) can push the chain output well
        // past 1.0 (seen at 1.7+ with high-gain NAM captures), which hard-clips at
        // the DAC and sounds like harsh distortion. Duck the block with a
        // fast-attack / slow-release gain so overs never reach the hardware; the
        // signal is untouched unless it actually exceeds the ceiling.
        float blockPeak = 0.0f;
        for (int32_t i = 0; i < block; ++i) {
            float l = std::fabs(processOutLeft_[i]);
            float r = std::fabs(processOutRight_[i]);
            float s = l > r ? l : r;
            if (s > blockPeak) blockPeak = s;
        }
        if (blockPeak > kLimiterCeiling) {
            float target = kLimiterCeiling / blockPeak;
            limiterGain_ += (target - limiterGain_) * kLimiterAttack;
        } else {
            // release slowly back to unity
            limiterGain_ += (1.0f - limiterGain_) * kLimiterRelease;
        }
        if (limiterGain_ < 1.0f) {
            // Rate-limited debug so a loud sustained note doesn't spam the log
            static auto lastLogLim = std::chrono::steady_clock::time_point{};
            auto nowLim = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(nowLim - lastLogLim).count() >= 1.0) {
                lastLogLim = nowLim;
                LOGW("Limiter engaged: blockPeak=%.3f gain=%.3f", blockPeak, limiterGain_);
            }
            for (int32_t i = 0; i < block; ++i) {
                processOutLeft_[i] *= limiterGain_;
                processOutRight_[i] *= limiterGain_;
            }
        }

        // Output peak metering (from buffers we wrote to)
        float outputPeak = 0.0f;
        bool outputClip = false;
        for (int32_t i = 0; i < block; ++i) {
            float s = std::max(std::fabs(processOutLeft_[i]), std::fabs(processOutRight_[i]));
            if (s > outputPeak) outputPeak = s;
            if (s >= kClippingThreshold) outputClip = true;
        }
        outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
        outputPeakLevel_.store(outputPeakHold_);
        if (outputClip) outputClipping_.store(true);

        // Feed recorder (lock-free ring buffer write)
        if (recorder_.isRecording()) {
            recorder_.feedAudio(processIn_.data(),
                                processOutLeft_.data(),
                                processOutRight_.data(),
                                block);
        }

        // Push the processed block (interleaved stereo) to the output ring so
        // the callback can drain it on its own cadence.
        outputRing_.writeInterleaved(processOutLeft_.data(), processOutRight_.data(), block);
    }

    // ---- Output side: drain the processed ring into the output buffer ----
    float* outData = static_cast<float*>(outputData);
    int32_t numChannels = outputStream_ ? outputStream_->getChannelCount() : 2;
    const size_t outNeeded = static_cast<size_t>(numFrames) * 2;  // interleaved floats
    if (outputRing_.readable() >= outNeeded) {
        if (numChannels == 2) {
            outputRing_.read(outData, outNeeded);
        } else {
            if (processInterleaved_.size() < outNeeded) {
                processInterleaved_.resize(outNeeded);
            }
            outputRing_.read(processInterleaved_.data(), outNeeded);
            for (int32_t i = 0; i < numFrames; ++i) {
                outData[i] = (processInterleaved_[i * 2] + processInterleaved_[i * 2 + 1]) * 0.5f;
            }
        }
    } else {
        // Ring not yet filled (startup window) or an input stall: output silence
        // rather than stale audio. The ring target pre-fill at start() covers the
        // normal startup path, so this is a safety net.
        outputUnderrunCount_.fetch_add(1, std::memory_order_relaxed);
        std::memset(outData, 0, outNeeded * sizeof(float));
    }

    // Rate-limited debug: output level + ring levels (once per second)
    {
        static auto lastLogOut = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogOut).count() >= 1.0) {
            lastLogOut = now;
            LOGI("onBothStreamsReady: outputPeak=%.4f ringIn=%zu ringOut=%zu",
                 outputPeakLevel_.load(), inputRing_.readable(), outputRing_.readable());
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
    // 华为/荣耀等设备的 AAudio 输入不支持独占/MMAP，打开后会被静默降级为共享
    // （深缓冲 → 数十 ms 延迟）。这些设备需走 OpenSL ES 的 FAST path 才能获得
    // 独占低延迟输入，因此 AAudio 打开后若发现非独占，改用 OpenSL ES 重开。
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
    // 华为等设备：AAudio 输入拿不到独占（或直接打不开）→ 改用 OpenSL ES。
    // OpenSL ES 输入在 API 23+ 支持 Float，直接用 Float 打开（与 FullDuplexStream
    // 的 float 读取匹配，切勿降级为 I16，否则回调里的数据会被当成 float 错乱）。
    bool triedOpenSLES = false;
    if (result == oboe::Result::OK &&
        inputStreamPtr->getSharingMode() != oboe::SharingMode::Exclusive) {
        LOGW("Input AAudio got non-exclusive sharing=%d; retrying with OpenSL ES for FAST path",
             static_cast<int>(inputStreamPtr->getSharingMode()));
        inputStreamPtr->close();
        inputStreamPtr = nullptr;
        result = openInput(inputDeviceId_, oboe::AudioApi::OpenSLES,
                           requestedRate, oboe::InputPreset::Generic);
        triedOpenSLES = true;
        // OpenSL ES 可能也拿不到独占 FAST path（共享模式），但不要回退 AAudio：
        // AAudio 共享输入会被系统强制 4096 帧深缓冲（约 85ms @48kHz），延迟
        // 不可接受。OpenSL ES 允许小缓冲（低延迟），共享模式下调度抖动导致的
        // xrun 杂音由 LatencyTuner 用更保守的输入缓冲目标（2×burst）来吸收。
        if (result == oboe::Result::OK) {
            LOGW("OpenSL ES input opened (sharing=%d perf=%d); relaxed input buffer target",
                 static_cast<int>(inputStreamPtr->getSharingMode()),
                 static_cast<int>(inputStreamPtr->getPerformanceMode()));
        }
    }
    if (result != oboe::Result::OK && !triedOpenSLES) {
        LOGI("Input AAudio open failed (%s); retrying with OpenSL ES",
             oboe::convertToText(result));
        result = openInput(inputDeviceId_, oboe::AudioApi::OpenSLES,
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
         // isMMapUsed() casts the stream to AAudio internally and would
         // null-deref if the stream ended up on OpenSL ES (Huawei/Xiaomi
         // fallback for exclusive input), so only query it for AAudio streams.
         isMMapUsedSafe(inputStream_.get()),
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
           // Do NOT allocate a session on the first attempt: on Xiaomi a
           // session forces the track onto the slow music-mixer path (deep
           // buffer, perf=None, 3844 frames ≈ 80ms) where OEM post-processing
           // gets inserted. The session-less fast track bypasses both.
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

    // If the session-less open was denied the low-latency path, the track sits
    // on the slow mixer where OEM post-processing (Dolby/MiSound/Volume
    // listener) is inserted and audibly degrades a live guitar signal. Reopen
    // WITH a session in that case so the Kotlin AudioEffectDisabler can strip
    // the degrading effects off the session.
    if (outputStreamPtr->getPerformanceMode() != oboe::PerformanceMode::LowLatency) {
        LOGW("Output without session got slow path (perf=%d); reopening with session for effect cleanup",
             static_cast<int>(outputStreamPtr->getPerformanceMode()));
        outputStreamPtr->close();
        outputBuilder.setAudioApi(oboe::AudioApi::AAudio)
                     ->setSessionId(oboe::SessionId::Allocate);
        oboe::AudioStream* s2 = nullptr;
        oboe::Result r2 = outputBuilder.openStream(&s2);
        if (r2 == oboe::Result::OK) {
            outputStreamPtr = s2;
            LOGI("Output reopened with session: api=%d sharing=%d perf=%d mmap=%d",
                 static_cast<int>(outputStreamPtr->getAudioApi()),
                 static_cast<int>(outputStreamPtr->getSharingMode()),
                 static_cast<int>(outputStreamPtr->getPerformanceMode()),
                 isMMapUsedSafe(outputStreamPtr));
        } else {
            // Session reopen refused — reopen the session-less stream so audio
            // still works (effects may be inserted; better than silence).
            LOGW("Reopen with session failed (%s); keeping session-less stream",
                 oboe::convertToText(r2));
            outputBuilder.setSessionId(oboe::SessionId::None);
            r2 = outputBuilder.openStream(&s2);
            if (r2 != oboe::Result::OK) {
                LOGE("Output reopen without session also failed (%s)",
                     oboe::convertToText(r2));
                closeStreams();
                return false;
            }
            outputStreamPtr = s2;
        }
    }

    LOGI("Output stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(outputStreamPtr->getAudioApi()),
         static_cast<int>(outputStreamPtr->getSharingMode()),
         static_cast<int>(outputStreamPtr->getPerformanceMode()),
         isMMapUsedSafe(outputStreamPtr));

    // Determine callback block size: always the hardware burst.
    // Processing at the burst size lets the stream buffers be sized as a small
    // multiple of it (see below). LV2 convolver plugins already round the block
    // size up to a power of 2 internally (LV2Plugin::activate), so we don't need
    // a power-of-2 framesPerCallback here — forcing one via setFramesPerCallback
    // only inflates the minimum buffer and adds latency.
    {
        int32_t framesPerBurst = outputStreamPtr->getFramesPerBurst();
        callbackFrameCount_ = (framesPerBurst > 0)
                                  ? static_cast<uint32_t>(framesPerBurst)
                                  : 128u;
        LOGI("Callback block = native burst: %u frames", callbackFrameCount_);
    }

    outputStream_.reset(outputStreamPtr);

    // Full-duplex 同步：把输入/输出流交给 FullDuplexStream 对齐。启动时会先排空
    // 输入缓冲并维持输入/输出均衡，回调内只在实际有数据时才读取，避免读到陈旧
    // 数据（USB 声卡场景下陈旧数据会累积成几十~上百毫秒的恒定延迟）。
    setInputStream(inputStream_.get());
    setOutputStream(outputStream_.get());

    // 输出缓冲 = N × 硬件 burst：用倍数表达缓冲，档位语义在任何设备上一致
    // （burst 大的设备缓冲绝对值也大，不会出现「同一个帧数在一台设备上够用、
    // 在另一台上持续欠载」）。Oboe 会把请求夹到流容量上限，实际值以返回值与
    // getBufferSizeInFrames() 为准。
    {
        int32_t outBurst = outputStream_->getFramesPerBurst();
        if (outBurst <= 0) outBurst = static_cast<int32_t>(callbackFrameCount_);
        int32_t targetOut = outBurst * requestedBufferBursts_;
        if (targetOut > 0) {
            auto shrink = outputStream_->setBufferSizeInFrames(targetOut);
            if (shrink == oboe::Result::OK) {
                LOGI("Output buffer set to %d frames (burst=%d x %d)",
                     shrink.value(), outBurst, requestedBufferBursts_);
            } else {
                LOGW("Failed to set output buffer to %d frames (burst=%d x %d): %s",
                     targetOut, outBurst, requestedBufferBursts_,
                     oboe::convertToText(shrink.error()));
            }
        }

        if (inputStream_) {
            int32_t inBurst = inputStream_->getFramesPerBurst();
            // OpenSL ES 共享输入调度不稳（小米 PerfSense 显示 HAL 周期 10-13ms，
            // 预期 2ms），输入缓冲放宽到 4×burst（≈16ms）吸收周期抖动，避免
            // 周期性欠载导致的滋滋声；AAudio 独占/低延迟流仍按 burst 收紧。
            // Input Cushion 由输入环预填实现，不在这里动 HAL 输入缓冲。
            int32_t inTarget = (inputStream_->getAudioApi() == oboe::AudioApi::OpenSLES)
                                   ? inBurst * 4
                                   : inBurst;
            int32_t minIn = std::max(inTarget, static_cast<int32_t>(callbackFrameCount_));
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
    // 应用 setBufferSizeInFrames。二次请求同一档位（N × burst）的缓冲，把应用
    // 侧停留时间压到档位允许的最低值。
    {
        int32_t outBurst = outputStream_->getFramesPerBurst();
        int32_t outTarget = outBurst * requestedBufferBursts_;
        if (outBurst > 0 && outputStream_->getBufferSizeInFrames() > outTarget) {
            auto r = outputStream_->setBufferSizeInFrames(outTarget);
            if (r == oboe::Result::OK) {
                LOGI("Post-start output resize to %d frames (burst=%d x %d)",
                     r.value(), outBurst, requestedBufferBursts_);
            } else {
                LOGW("Post-start output resize to %d failed: %s",
                     outTarget, oboe::convertToText(r.error()));
            }
        }
        if (inputStream_) {
            int32_t inBurst = inputStream_->getFramesPerBurst();
            // OpenSL ES 共享输入用 2×burst 作为缓冲目标（同初始收缩逻辑）。
            int32_t inTarget = (inputStream_->getAudioApi() == oboe::AudioApi::OpenSLES)
                                   ? inBurst * 2
                                   : inBurst;
            if (inBurst > 0 && inputStream_->getBufferSizeInFrames() > inTarget) {
                auto r = inputStream_->setBufferSizeInFrames(inTarget);
                if (r == oboe::Result::OK) {
                    LOGI("Post-start input shrink to %d frames (burst=%d)", r.value(), inBurst);
                } else {
                    LOGW("Post-start input shrink to %d failed: %s",
                         inTarget, oboe::convertToText(r.error()));
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

    // 详细诊断日志：用于核对 USB 声卡场景下实际协商出的采样率/缓冲/共享模式。
    LOGI("AUDIO STREAM CFG: rate=%d callbackFrames=%u "
         "in{api=%d sharing=%d perf=%d mmap=%d burst=%d buf=%d cap=%d} "
         "out{api=%d sharing=%d perf=%d mmap=%d burst=%d buf=%d cap=%d}",
         static_cast<int>(sampleRate_), callbackFrameCount_,
         static_cast<int>(inputStream_->getAudioApi()),
         static_cast<int>(inputStream_->getSharingMode()),
         static_cast<int>(inputStream_->getPerformanceMode()),
         isMMapUsedSafe(inputStream_.get()),
         inputStream_->getFramesPerBurst(),
         inputStream_->getBufferSizeInFrames(),
         inputStream_->getBufferCapacityInFrames(),
         static_cast<int>(outputStream_->getAudioApi()),
         static_cast<int>(outputStream_->getSharingMode()),
         static_cast<int>(outputStream_->getPerformanceMode()),
         isMMapUsedSafe(outputStream_.get()),
         outputStream_->getFramesPerBurst(),
         outputStream_->getBufferSizeInFrames(),
         outputStream_->getBufferCapacityInFrames());
    LOGI("Audio streams created: %d Hz, buffer size: %d frames",
         static_cast<int>(sampleRate_), outputStream_->getBufferSizeInFrames());

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

        // Output: keep retrying to shrink toward the selected tier (N x burst).
        // Non-MMAP paths can hand out a huge burst equal to the initial buffer
        // (e.g. 3844 frames ≈ 80ms), which the one-shot create-time resize
        // would never touch. Asking below the burst is allowed on some
        // implementations; if rejected we just log the clamp and give up after
        // a few consecutive rejections (the policy won't relax).
        auto* out = outputStream_.get();
        if (out && !outputTuned_) {
            int32_t outCurrent = out->getBufferSizeInFrames();
            int32_t outBurst = out->getFramesPerBurst();
            if (outBurst <= 0) outBurst = static_cast<int32_t>(callbackFrameCount_);
            int32_t outTarget = outBurst * requestedBufferBursts_;
            if (outCurrent > outTarget) {
                auto ro = out->setBufferSizeInFrames(outTarget);
                if (ro == oboe::Result::OK && ro.value() < outCurrent) {
                    LOGI("LatencyTuner: output buffer %d -> %d frames (target=%d)",
                         outCurrent, ro.value(), outTarget);
                    outputClampStreak_ = 0;
                    tunerXrunsAtShrink_.store(getXRunCount());
                    nap(2000);
                    continue;
                }
                if (ro == oboe::Result::OK) {
                    LOGW("LatencyTuner: output shrink to %d clamped at %d frames (burst=%d)",
                         outTarget, ro.value(), out->getFramesPerBurst());
                } else {
                    LOGW("LatencyTuner: output shrink to %d failed: %s",
                         outTarget, oboe::convertToText(ro.error()));
                }
                if (++outputClampStreak_ >= 3) {
                    LOGI("LatencyTuner: output stuck at %d frames (system clamp), giving up",
                         outCurrent);
                    outputTuned_ = true;
                }
            } else {
                LOGI("LatencyTuner: output buffer at target %d frames, tuning done", outCurrent);
                outputTuned_ = true;
            }
        }

        int32_t inBurst = in->getFramesPerBurst();
        // OpenSL ES 共享输入调度不稳（HAL 周期 10-13ms vs 预期 2ms），用 4×burst
        // 作为收缩目标（≈16ms 抖动吸收），与 createAudioStreams 一致。
        int32_t inTarget = (in->getAudioApi() == oboe::AudioApi::OpenSLES)
                               ? inBurst * 4
                               : inBurst;
        int32_t target = std::max(inTarget, static_cast<int32_t>(callbackFrameCount_));
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
                inputClampStreak_ = 0;
                tunerXrunsAtShrink_.store(xruns);
                nap(2000);
                continue;
            }
            if (r == oboe::Result::OK && r.value() >= current) {
                // Accepted but clamped by the audio policy — the requested
                // size is below the enforced minimum. Retry a couple of times
                // in case the policy relaxes, then give up.
                LOGW("LatencyTuner: input shrink to %d clamped at %d frames "
                     "(system-enforced minimum)", target, r.value());
                if (++inputClampStreak_ >= 3) {
                    LOGI("LatencyTuner: input stuck at %d frames (system clamp), giving up",
                         current);
                    inputTuned_ = true;
                }
            }
        } else if (current <= target) {
            // Fully shrunk. If the last shrink stayed xrun-free across the
            // observation window, tuning is done; otherwise the backoff
            // branch above will have relaxed the buffer already.
            LOGI("LatencyTuner: input buffer at target %d frames, tuning done", target);
            break;
        }
        if (outputTuned_ && inputTuned_) {
            LOGI("LatencyTuner: both streams at system limit, tuning done");
            break;
        }
        nap(1000);
    }

    // Monitor phase: tuning is over (or gave up), but heavy plugins (NAM etc.)
    // can still overload a tiny callback buffer later. Watch the xrun counter
    // once a second; on persistent underruns, grow the output buffer by one
    // burst so the mixer has headroom to absorb callback jitter. This directly
    // targets the "破音/掉帧" symptom from underruns at small buffer sizes.
    if (latencyTunerRunning_.load()) {
        LOGI("LatencyTuner: entering underrun-monitor phase");
    }
    while (latencyTunerRunning_.load()) {
        nap(1000);
        auto* out = outputStream_.get();
        if (!out) break;
        int32_t now = getXRunCount();
        if (lastMonitoredXruns_ < 0) {
            lastMonitoredXruns_ = now;
            continue;
        }
        int32_t delta = now - lastMonitoredXruns_;
        lastMonitoredXruns_ = now;
        if (delta < 2) continue;  // <2 xruns/sec is noise; leave it alone

        int32_t cur = out->getBufferSizeInFrames();
        int32_t burst = out->getFramesPerBurst();
        if (burst <= 0) burst = 64;
        int32_t next = std::min(cur + burst, kMaxTunerBufferFrames);
        if (next > cur) {
            auto ro = out->setBufferSizeInFrames(next);
            if (ro == oboe::Result::OK && ro.value() > cur) {
                LOGW("AntiUnderrun: +%d xruns/s grew output buffer %d -> %d frames "
                     "(latency +%.1fms)",
                     delta, cur, ro.value(),
                     ((ro.value() - cur) / sampleRate_) * 1000.0);
            } else {
                LOGW("AntiUnderrun: grow to %d failed or clamped at %d",
                     next, ro == oboe::Result::OK ? ro.value() : cur);
            }
        }
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

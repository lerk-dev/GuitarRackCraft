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

#ifndef GUITARRACKCRAFT_AUDIO_ENGINE_H
#define GUITARRACKCRAFT_AUDIO_ENGINE_H

#include <oboe/Oboe.h>
#include <oboe/FullDuplexStream.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "plugin/PluginChain.h"
#include "AudioRecorder.h"
#include "Tuner.h"

namespace guitarrackcraft {

/**
 * Lock-free single-producer/single-consumer float ring buffer for the audio
 * thread. The engine decouples input arrival from DSP consumption through this
 * buffer (NAM Sandwich's approach): the Oboe callback drains the HAL input and
 * pushes it here; the DSP side consumes fixed-size blocks whenever enough data
 * is buffered, so an irregular HAL supply cadence (e.g. Redmi K80's 10-13ms
 * input delivery period vs the 4ms callback block) can never starve or stall
 * the plugin chain. Monotonic indices with modular access — safe because only
 * the audio thread touches it and capacity is never exceeded.
 */
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacityFrames)
        : buf_(capacityFrames, 0.0f) {}

    void reset() { readIdx_ = writeIdx_ = 0; }
    size_t readable() const { return writeIdx_ - readIdx_; }
    size_t writable() const { return buf_.size() - readable(); }
    size_t capacityFrames() const { return buf_.size(); }

    void write(const float* src, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            buf_[writeIdx_ % buf_.size()] = src[i];
            ++writeIdx_;
        }
    }

    void writeSilence(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            buf_[writeIdx_ % buf_.size()] = 0.0f;
            ++writeIdx_;
        }
    }

    /** Write nFrames of interleaved stereo (2 floats per frame). Used by the
     *  output ring so processed L/R never needs a separate interleave buffer. */
    void writeInterleaved(const float* left, const float* right, size_t nFrames) {
        for (size_t i = 0; i < nFrames; ++i) {
            buf_[writeIdx_ % buf_.size()] = left[i];
            buf_[(writeIdx_ + 1) % buf_.size()] = right[i];
            writeIdx_ += 2;
        }
    }

    void read(float* dst, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            dst[i] = buf_[readIdx_ % buf_.size()];
            ++readIdx_;
        }
    }

private:
    std::vector<float> buf_;
    size_t readIdx_ = 0;
    size_t writeIdx_ = 0;
};

/**
 * Audio engine using Oboe for low-latency audio I/O.
 * Processes audio through the plugin chain in real-time.
 *
 * Inherits oboe::FullDuplexStream so the input and output streams are
 * synchronized inside the output data callback: the input is drained on
 * startup and read only when data is actually available, so stale audio
 * never piles up in the input buffer (a common cause of large, constant
 * latency when using a USB audio interface).
 */
class AudioEngine : public oboe::FullDuplexStream,
                    public oboe::AudioStreamErrorCallback {
public:
    AudioEngine();
    ~AudioEngine();

    /**
     * Start audio processing.
     * @param sampleRate Desired sample rate (will use device default if not supported)
     * @param bufferBursts Output buffer size as a multiple of the hardware burst
     * @param inputCushionMs Extra input-side slack (ms) pre-filled into the input ring
     * @return true if started successfully
     */
    bool start(float sampleRate = 48000.0f, int32_t inputDeviceId = 0,
               int32_t outputDeviceId = 0, int32_t bufferBursts = 4,
               int32_t inputCushionMs = 0);

    /**
     * Stop audio processing.
     * Overrides oboe::FullDuplexStream::stop() — same signature, different
     * semantics: tears down recorder, plugin chain and both streams safely.
     */
    oboe::Result stop() override;

    /**
     * Check if engine is running.
     */
    bool isRunning() const;

    /**
     * Get the plugin chain (for adding/removing plugins).
     */
    PluginChain& getChain() { return chain_; }
    const PluginChain& getChain() const { return chain_; }

    /**
     * Get current sample rate.
     */
    float getSampleRate() const { return sampleRate_; }

    /**
     * Get actual callback frame count (buffer size used by the audio callback).
     */
    uint32_t getCallbackFrameCount() const { return callbackFrameCount_; }

    struct StreamInfo {
        bool isAAudio = false;         // AAudio vs OpenSL ES
        bool inputExclusive = false;   // Exclusive sharing mode granted
        bool outputExclusive = false;
        bool inputLowLatency = false;  // LowLatency performance mode granted
        bool outputLowLatency = false;
        bool outputMMap = false;       // MMAP used (lowest path)
        bool outputCallback = true;    // Using data callback
        int32_t framesPerBurst = 0;    // Hardware burst size
    };

    /**
     * Get stream configuration info for the low-latency checklist.
     */
    StreamInfo getStreamInfo() const;

    /**
     * Get current latency in milliseconds.
     */
    double getLatencyMs() const;

    /**
     * Get input peak level (0.0–1.0).
     */
    float getInputLevel() const;

    /**
     * Get output peak level (0.0–1.0).
     */
    float getOutputLevel() const;

    /**
     * Get CPU load (0.0–1.0) from processing time vs buffer duration.
     */
    float getCpuLoad() const;

    /**
     * Get cumulative audio xrun (underrun/overrun) count from the output stream.
     */
    int32_t getXRunCount() const;

    /**
     * Safe MMAP check: oboe's OboeExtensions::isMMapUsed() casts the stream to
     * AAudio internally and null-derefs if the stream ended up on OpenSL ES
     * (the Huawei/Xiaomi exclusive-input fallback). Only meaningful for AAudio.
     */
    static int32_t isMMapUsedSafe(oboe::AudioStream* stream);

    /**
     * Audio session id of the output stream (AAudio). Used by the Kotlin layer
     * to disable system post-processing effects (Dolby/MiSound) that OEM audio
     * policies insert on music streams and that badly distort guitar audio.
     * Returns 0 if the stream is not open or has no session.
     */
    int32_t getOutputSessionId() const;

    /**
     * True if input has clipped (peak >= 0.99).
     */
    bool isInputClipping() const;

    /**
     * True if output has clipped (peak >= 0.99).
     */
    bool isOutputClipping() const;

    /**
     * Clear clipping indicators (call when user taps to reset).
     */
    void resetClipping();

    /**
     * Bypass chain processing (audio passthrough). Use during preset loading
     * to prevent the audio thread from processing a partially-built chain.
     */
    void setChainBypass(bool bypass) { chainBypass_.store(bypass); }
    void setWavBypassChain(bool bypass) { wavBypassChain_.store(bypass); }

    // --- Pre-chain input gain + noise gate ---
    // Applied to the input buffer before metering and the plugin chain, so
    // the input meter shows what actually hits the chain (gain staging).

    /** Input pre-gain in dB, -24..+24 (0 = unity). */
    void setPreGainDb(float db);
    float getPreGainDb() const { return preGainDb_.load(); }

    /** Noise gate threshold in dBFS, -80..-20; <= -96 disables the gate. */
    void setGateThresholdDb(float db);
    float getGateThresholdDb() const { return gateThresholdDb_.load(); }

    /** Hysteresis above the threshold required to re-open the gate, 0..12 dB. */
    void setGateHysteresisDb(float db);
    float getGateHysteresisDb() const { return gateHysteresisDb_.load(); }

    /** Gain the gate closes down to, -100..-20 dB (never full silence). */
    void setGateFloorDb(float db);
    float getGateFloorDb() const { return gateFloorDb_.load(); }

    /** Envelope attack, 0.5..50 ms. Slow values reject sub-5ms input spikes. */
    void setGateAttackMs(float ms);
    float getGateAttackMs() const { return gateAttackMs_.load(); }

    /** Time below the threshold before the gate closes, 0..500 ms. */
    void setGateHoldMs(float ms);
    float getGateHoldMs() const { return gateHoldMs_.load(); }

    /** Gain close ramp time, 10..1000 ms. */
    void setGateReleaseMs(float ms);
    float getGateReleaseMs() const { return gateReleaseMs_.load(); }

    // --- Stream/buffer diagnostics (UI panel) ---

    int32_t getFramesPerBurst() const;
    int32_t getOutputBufferFrames() const;
    int32_t getOutputBufferCapacityFrames() const;
    int32_t getInputBufferFrames() const;
    int32_t getInputCushionFrames() const { return inputCushionFrames_; }
    int32_t getRingTargetFrames() const { return ringTargetFrames_; }
    int32_t getInputRingOverflows() const { return inputRingOverflowCount_.load(); }
    int32_t getOutputUnderruns() const { return outputUnderrunCount_.load(); }

    /** Post-chain output gain (master volume) in dB, -24..+24 (0 = unity). */
    void setOutputGainDb(float db);
    float getOutputGainDb() const { return outputGainDb_.load(); }

    // --- Tuner ---
    Tuner& getTuner() { return tuner_; }

    /**
     * Get the audio recorder for real-time recording of raw input and processed output.
     */
    AudioRecorder& getRecorder() { return recorder_; }

    // --- WAV real-time playback ---

    /**
     * Load a WAV file for playback. Engine must be running (sample rate known).
     * Converts to mono and resamples to engine rate.
     * @return true on success
     */
    bool loadWav(const std::string& path);

    /**
     * Unload the current WAV and stop playback.
     */
    void unloadWav();

    void wavPlay();
    void wavPause();
    void wavSeekToFrame(size_t frame);

    double getWavDurationSec() const;
    double getWavPositionSec() const;
    bool isWavPlaying() const;
    bool isWavLoaded() const;

    // FullDuplexStream callback (implemented): called from the output stream's
    // data callback once both input and output data are available.
    oboe::DataCallbackResult onBothStreamsReady(
        const void* inputData,
        int   numInputFrames,
        void* outputData,
        int   numOutputFrames
        ) override;

    // AudioStreamErrorCallback implementation
    void onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) override;

private:
    std::unique_ptr<oboe::AudioStream> inputStream_;
    std::unique_ptr<oboe::AudioStream> outputStream_;
    PluginChain chain_;
    float sampleRate_;
    int32_t inputDeviceId_ = 0;
    int32_t outputDeviceId_ = 0;
    int32_t requestedBufferBursts_ = 4;  // output buffer = N x hardware burst
    uint32_t callbackFrameCount_ = 0;  // frames per audio callback (= native burst)
    std::atomic<bool> isRunning_;
    std::atomic<bool> chainBypass_{false};  // skip chain processing (passthrough)

    // DSP block buffers (audio thread). The Oboe callback decouples input
    // arrival from plugin-chain consumption through inputRing_/outputRing_
    // (see AudioRingBuffer above): input is pushed as it arrives, and the
    // chain runs on fixed callback-sized blocks whenever enough data has
    // accumulated. This absorbs irregular HAL input supply (10-13ms periods
    // on some Xiaomi/Redmi devices) so the chain is never starved or gated
    // by the input cadence — the primary cause of the intermittent "buzz"
    // heard on the K80 even with silent input.
    std::vector<float> processIn_;
    std::vector<float> processOutLeft_;
    std::vector<float> processOutRight_;
    std::vector<float> processInterleaved_;  // mono-mix temp for mono output paths
    /** inputRing_: mono frames of raw input as delivered by the HAL.
     *  outputRing_: interleaved stereo floats (2 per frame) of processed audio. */
    AudioRingBuffer inputRing_{16384};
    AudioRingBuffer outputRing_{32768};
    /** Output-side ring fill target (frames): the constant latency the DSP
     *  path adds. Pre-filled at start() so the output never underflows while
     *  the first input burst is still traveling through the HAL. */
    int32_t ringTargetFrames_ = 0;
    static constexpr float kRingTargetMs = 12.0f;
    /** Extra input-side slack pre-filled into inputRing_ at start(), in frames.
     *  Adds a fixed input latency that absorbs HAL scheduling jitter. */
    int32_t inputCushionFrames_ = 0;
    /** Diagnostics: input ring writes dropped (ring full) and output ring
     *  underruns (silence substituted for processed audio). */
    std::atomic<int32_t> inputRingOverflowCount_{0};
    std::atomic<int32_t> outputUnderrunCount_{0};
    
    // Temporary buffers for plugin chain
    const float* inputPtrs_[2];
    float* outputPtrs_[2];

    // Level metering and CPU (written from audio thread, read from UI)
    std::atomic<float> inputPeakLevel_{0.0f};
    std::atomic<float> outputPeakLevel_{0.0f};
    std::atomic<float> cpuLoad_{0.0f};
    std::atomic<bool> inputClipping_{false};
    std::atomic<bool> outputClipping_{false};
    float inputPeakHold_{0.0f};
    float outputPeakHold_{0.0f};

    // Aggregated subprocess (wine VST) load + xruns. Sampled periodically
    // from the audio callback (every ~1s) so the UI's getCpuLoad / xrun
    // metrics include VSTs that run out-of-process. Without this, a Helix
    // Native that's saturating the wine subprocess shows 1% CPU and 0
    // xruns because the audio thread itself is idle — the wine subprocess
    // is the bottleneck and it isn't measured here.
    std::atomic<float>   vstCpuLoad_{0.0f};
    std::atomic<int32_t> vstUnderruns_{0};
    /** Per-plugin last-sampled jiffies counter, keyed by subprocess pid.
     *  Lives in audio-callback context — only touched at periodic-sample
     *  cadence (not every callback). */
    std::unordered_map<int, uint64_t> vstLastJiffies_;
    /** Wall-time when vstLastJiffies_ was last updated (clock_gettime ns). */
    uint64_t vstLastSampleNs_{0};
    /** Audio-callback counter so we sample subprocesses every N callbacks
     *  (~1Hz) rather than every block — reading /proc is cheap but not
     *  free, and the value is for human-facing UI. */
    uint32_t vstSampleCounter_{0};

    static constexpr float kClippingThreshold = 0.99f;
    static constexpr float kPeakDecay = 0.95f;

    // Output soft limiter (audio thread): hot amp models (NAM) can push the
    // chain output well past 1.0 and hard-clip at the DAC. Duck with a
    // fast-attack / slow-release gain so overs never reach the hardware.
    static constexpr float kLimiterCeiling = 0.98f;
    static constexpr float kLimiterAttack = 0.3f;   // per-block gain step down
    static constexpr float kLimiterRelease = 0.01f; // per-block gain step up (fast enough to avoid pumping on sustained notes)
    float limiterGain_{1.0f};

    // Pre-chain input gain + noise gate state (audio thread)
    std::atomic<float> preGainDb_{0.0f};
    std::atomic<float> gateThresholdDb_{-60.0f};  // <= -96 dB: gate disabled
    std::atomic<float> gateHysteresisDb_{3.0f};   // open needs threshold + N dB
    std::atomic<float> gateFloorDb_{-80.0f};      // gain when closed (never 0)
    std::atomic<float> gateAttackMs_{5.0f};       // envelope attack
    std::atomic<float> gateHoldMs_{50.0f};        // below threshold this long before closing
    std::atomic<float> gateReleaseMs_{100.0f};    // gain close ramp
    float preGainLin_{1.0f};       // cached linear gain (audio thread only)
    float preGainDbCached_{0.0f};  // dB value preGainLin_ was computed from
    float gateEnv_{0.0f};          // peak envelope follower for the gate
    float gateGain_{1.0f};         // smoothed gate open/close gain
    // Gate hysteresis + hold: the gate only closes when the envelope stays
    // below the close threshold for a full hold window. Opening requires the
    // envelope to rise gateHysteresisDb_ above the threshold. This stops the
    // gate from flapping (buzz) or abruptly cutting a naturally decaying note.
    // The hold window is deliberately short: 400ms let a decaying note fall
    // below the threshold while the gate stayed fully open — on a high-gain NAM
    // the amplified floor noise kept passing for the whole window, sounding like
    // a burst of noise for ~0.5s after the player stops picking. 50ms is still
    // longer than a string's natural decay down to the threshold, so sustained
    // notes are not chopped, but the noise tail after stopping is inaudible.
    bool gateOpen_{false};
    int32_t gateHoldFrames_{0};
    void applyPreGainAndGate(float* buf, int32_t numFrames);

    // Post-chain output gain (master volume) state (audio thread)
    std::atomic<float> outputGainDb_{0.0f};
    float outputGainLin_{1.0f};       // cached linear gain (audio thread only)
    float outputGainDbCached_{0.0f};  // dB value outputGainLin_ was computed from

    // Tuner: fed from the audio callback, analyzed on its own worker thread
    Tuner tuner_;

    // WAV playback state (read in callback; written from load/seek/play/pause)
    std::vector<float> wavBuffer_;
    std::atomic<size_t> wavPositionFrames_{0};
    std::atomic<bool> wavPlaying_{false};
    std::atomic<bool> wavBypassChain_{true};  // true = WAV plays raw (backing track), false = through effects
    size_t wavLengthFrames_{0};

    AudioRecorder recorder_;

    // Adaptive latency tuner (Amp Rack / LatencyTuner approach): periodically
    // retries shrinking the input buffer toward the burst size — some devices
    // only accept it once the stream has been running for a while — and backs
    // off by one burst when new xruns appear after a shrink. Also keeps
    // retrying the output shrink: on non-MMAP paths the system can hand out a
    // huge burst (e.g. 3844 frames ≈ 80ms) that equals the initial buffer
    // size, so the one-shot create-time shrink would never fire.
    std::thread latencyTunerThread_;
    std::atomic<bool> latencyTunerRunning_{false};
    std::atomic<int32_t> tunerXrunsAtShrink_{-1};
    bool outputTuned_ = false;
    bool inputTuned_ = false;
    int outputClampStreak_ = 0;   // consecutive system-clamped output shrinks
    int inputClampStreak_ = 0;    // consecutive system-clamped input shrinks
    int32_t lastMonitoredXruns_ = -1;  // xrun baseline for the underrun monitor
    static constexpr int32_t kMaxTunerBufferFrames = 1024;
    void latencyTunerLoop();

    bool createAudioStreams(float sampleRate);
    void closeStreams();
    void resampleToEngineRate(const std::vector<float>& src, uint32_t srcRate,
                              std::vector<float>& dst);
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_ENGINE_H

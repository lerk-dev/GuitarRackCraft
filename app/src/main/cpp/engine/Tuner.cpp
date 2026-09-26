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

#include "Tuner.h"
#include <chrono>
#include <cmath>
#include <cstring>

namespace guitarrackcraft {

namespace {
// Analysis window: 2048 @48kHz ≈ 43 ms — covers ~3.5 periods of low E2 (82 Hz).
constexpr int kWindowSize = 2048;
// Guitar range with margin: 60 Hz .. 1300 Hz
constexpr float kMinFreq = 60.0f;
constexpr float kMaxFreq = 1300.0f;
// NSDF peak acceptance threshold (MPM uses 0.8..0.9; lower = more sensitive)
constexpr float kClarityThreshold = 0.75f;
// Below this RMS the signal is treated as silence (~-50 dBFS)
constexpr float kSilenceRms = 0.003f;
// Analysis cadence
constexpr auto kAnalysisPeriod = std::chrono::milliseconds(60);
} // namespace

Tuner::Tuner() : ring_(kRingSize, 0.0f) {}

Tuner::~Tuner() {
    setEnabled(false);
}

void Tuner::setSampleRate(float sampleRate) {
    sampleRate_.store(sampleRate);
}

void Tuner::setEnabled(bool enabled) {
    bool was = enabled_.exchange(enabled);
    if (enabled && !was) {
        // Reset state and start the worker
        ringWritePos_.store(0);
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        frequency_.store(0.0f);
        clarity_.store(0.0f);
        lastFreq_ = 0.0f;
        silentFrames_ = 0;
        workerRunning_.store(true);
        workerThread_ = std::thread(&Tuner::workerLoop, this);
    } else if (!enabled && was) {
        workerRunning_.store(false);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        frequency_.store(0.0f);
        clarity_.store(0.0f);
    }
}

void Tuner::pushSamples(const float* data, int32_t numFrames) {
    if (!enabled_.load(std::memory_order_relaxed) || numFrames <= 0) return;
    uint32_t pos = ringWritePos_.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < numFrames; ++i) {
        ring_[pos] = data[i];
        pos = (pos + 1) & (kRingSize - 1);
    }
    ringWritePos_.store(pos, std::memory_order_release);
}

void Tuner::workerLoop() {
    std::vector<float> window(kWindowSize);
    while (workerRunning_.load()) {
        // Copy the most recent kWindowSize samples from the ring.
        uint32_t writePos = ringWritePos_.load(std::memory_order_acquire);
        uint32_t start = (writePos - kWindowSize) & (kRingSize - 1);
        if (start + kWindowSize <= kRingSize) {
            std::memcpy(window.data(), ring_.data() + start, kWindowSize * sizeof(float));
        } else {
            uint32_t first = kRingSize - start;
            std::memcpy(window.data(), ring_.data() + start, first * sizeof(float));
            std::memcpy(window.data() + first, ring_.data(), (kWindowSize - first) * sizeof(float));
        }

        // Silence check (RMS)
        float sumSq = 0.0f;
        for (int i = 0; i < kWindowSize; ++i) sumSq += window[i] * window[i];
        float rms = std::sqrt(sumSq / kWindowSize);

        if (rms < kSilenceRms) {
            // Require a few consecutive silent windows before clearing the
            // display so the needle doesn't flicker between notes.
            if (++silentFrames_ >= 4) {
                frequency_.store(0.0f);
                clarity_.store(0.0f);
                lastFreq_ = 0.0f;
            }
        } else {
            silentFrames_ = 0;
            float clarity = 0.0f;
            float freq = detectPitch(window.data(), kWindowSize, clarity);
            if (freq > 0.0f) {
                // Smooth: small corrections glide, note changes snap.
                if (lastFreq_ > 0.0f) {
                    float ratio = freq / lastFreq_;
                    if (ratio > 0.943f && ratio < 1.06f) {  // within one semitone
                        freq = lastFreq_ * 0.6f + freq * 0.4f;
                    }
                }
                lastFreq_ = freq;
                frequency_.store(freq);
                clarity_.store(clarity);
            } else {
                frequency_.store(0.0f);
                clarity_.store(0.0f);
            }
        }

        std::this_thread::sleep_for(kAnalysisPeriod);
    }
}

/**
 * McLeod Pitch Method: build the normalized square difference function
 * (NSDF) and pick the first peak above the clarity threshold. Parabolic
 * interpolation around the peak gives sub-sample period accuracy.
 */
float Tuner::detectPitch(const float* x, int n, float& clarityOut) {
    const float sr = sampleRate_.load();
    const int minLag = static_cast<int>(sr / kMaxFreq);   // ~36 @48k
    const int maxLag = static_cast<int>(sr / kMinFreq);   // ~800 @48k
    const int usable = n - maxLag;
    if (usable <= 0) return 0.0f;

    // NSDF values for lags in [minLag, maxLag]
    std::vector<float> nsdf(maxLag + 1, 0.0f);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        float acf = 0.0f;   // sum x[j] * x[j+lag]
        float div = 0.0f;   // sum x[j]^2 + x[j+lag]^2
        for (int j = 0; j < usable; ++j) {
            acf += x[j] * x[j + lag];
            div += x[j] * x[j] + x[j + lag] * x[j + lag];
        }
        nsdf[lag] = (div > 1e-9f) ? (2.0f * acf / div) : 0.0f;
    }

    // Find peaks (local maxima) and take the first one above threshold;
    // fall back to the global maximum if none qualifies.
    int bestLag = -1;
    float bestVal = 0.0f;
    int globalLag = minLag;
    float globalVal = -1.0f;
    for (int lag = minLag + 1; lag < maxLag; ++lag) {
        float v = nsdf[lag];
        if (v > globalVal) { globalVal = v; globalLag = lag; }
        if (v > nsdf[lag - 1] && v >= nsdf[lag + 1]) {  // local maximum
            if (v >= kClarityThreshold) { bestLag = lag; bestVal = v; break; }
        }
    }
    if (bestLag < 0) {
        if (globalVal < 0.5f) return 0.0f;  // no periodicity at all
        bestLag = globalLag;
        bestVal = globalVal;
    }

    // Parabolic interpolation around the peak
    float y0 = nsdf[bestLag - 1];
    float y1 = nsdf[bestLag];
    float y2 = nsdf[bestLag + 1];
    float denom = (y0 - 2.0f * y1 + y2);
    float shift = (std::fabs(denom) > 1e-9f) ? 0.5f * (y0 - y2) / denom : 0.0f;
    float preciseLag = static_cast<float>(bestLag) + shift;
    if (preciseLag <= 0.0f) return 0.0f;

    clarityOut = bestVal;
    return sr / preciseLag;
}

} // namespace guitarrackcraft

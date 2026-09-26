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

#ifndef GUITARRACKCRAFT_TUNER_H
#define GUITARRACKCRAFT_TUNER_H

#include <atomic>
#include <thread>
#include <vector>

namespace guitarrackcraft {

/**
 * Real-time pitch detector for the tuner screen.
 *
 * The audio callback pushes input samples into a lock-free ring buffer
 * (only while enabled); a worker thread periodically runs the McLeod
 * Pitch Method (NSDF autocorrelation) over the most recent window and
 * publishes the detected frequency. This keeps all heavy math off the
 * real-time audio thread.
 */
class Tuner {
public:
    Tuner();
    ~Tuner();

    void setSampleRate(float sampleRate);

    /** Enable/disable detection. Starts/stops the worker thread. */
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_.load(); }

    /** Called from the audio callback with mono input samples. */
    void pushSamples(const float* data, int32_t numFrames);

    /** Detected frequency in Hz, 0 when no reliable pitch. */
    float getFrequency() const { return frequency_.load(); }

    /** Detection clarity 0..1 (NSDF peak height). */
    float getClarity() const { return clarity_.load(); }

private:
    void workerLoop();
    float detectPitch(const float* window, int n, float& clarityOut);

    std::atomic<float> sampleRate_{48000.0f};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> workerRunning_{false};
    std::thread workerThread_;

    // Single-producer (audio thread) single-consumer (worker) ring buffer.
    static constexpr uint32_t kRingSize = 8192;  // power of 2
    std::vector<float> ring_;
    std::atomic<uint32_t> ringWritePos_{0};

    std::atomic<float> frequency_{0.0f};
    std::atomic<float> clarity_{0.0f};

    // Worker-side smoothing state
    float lastFreq_{0.0f};
    int silentFrames_{0};
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_TUNER_H

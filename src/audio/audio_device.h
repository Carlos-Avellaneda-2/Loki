#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace loki::audio {

// Fills `out` with `frames` interleaved float frames. Runs on the real-time audio thread:
// no locks, no allocation, no I/O.
using RenderCallback = std::function<void(float* out, std::size_t frames)>;

// Audio output abstraction (miniaudio in the app, a manual device in tests).
class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    AudioDevice(AudioDevice&&) = delete;
    AudioDevice& operator=(AudioDevice&&) = delete;

    virtual bool start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;
    [[nodiscard]] virtual int sampleRate() const = 0;
    [[nodiscard]] virtual int channels() const = 0;
    // Frames between the callback and the speaker (buffering); used to align video to sound.
    [[nodiscard]] virtual std::size_t latencyFrames() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    AudioDevice() = default;
};

using AudioDeviceFactory = std::function<std::unique_ptr<AudioDevice>(RenderCallback callback)>;

// Default playback device through miniaudio (48 kHz stereo float). Returns nullptr when no
// output device is available (the caller then plays video against the wall clock).
[[nodiscard]] std::unique_ptr<AudioDevice> createDefaultDevice(RenderCallback callback, int sampleRate = 48000,
                                                               int channels = 2);

// Test device: nothing plays; pump() invokes the callback synchronously.
class ManualAudioDevice final : public AudioDevice {
public:
    explicit ManualAudioDevice(RenderCallback callback, int sampleRate = 48000, int channels = 2,
                               std::size_t latency = 0)
        : m_callback(std::move(callback)), m_rate(sampleRate), m_channels(channels), m_latency(latency) {}

    bool start() override {
        m_running = true;
        return true;
    }
    void stop() override { m_running = false; }
    [[nodiscard]] bool isRunning() const override { return m_running; }
    [[nodiscard]] int sampleRate() const override { return m_rate; }
    [[nodiscard]] int channels() const override { return m_channels; }
    [[nodiscard]] std::size_t latencyFrames() const override { return m_latency; }
    [[nodiscard]] std::string name() const override { return "manual"; }

    // Requests `frames` frames like a real device would; returns what the callback produced.
    std::vector<float> pump(std::size_t frames) {
        std::vector<float> out(frames * static_cast<std::size_t>(m_channels), 0.0f);
        if (m_running && m_callback) {
            m_callback(out.data(), frames);
        }
        return out;
    }

private:
    RenderCallback m_callback;
    int m_rate;
    int m_channels;
    std::size_t m_latency;
    bool m_running = false;
};

} // namespace loki::audio

// miniaudio implementation lives in exactly one translation unit.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "audio/audio_device.h"

#include <spdlog/spdlog.h>

#include <atomic>

namespace loki::audio {

namespace {

class MiniaudioDevice final : public AudioDevice {
public:
    MiniaudioDevice(RenderCallback callback, int sampleRate, int channels) : m_callback(std::move(callback)) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = static_cast<ma_uint32>(channels);
        config.sampleRate = static_cast<ma_uint32>(sampleRate);
        config.periodSizeInMilliseconds = 10; // low latency, still safe on slow machines
        config.dataCallback = &MiniaudioDevice::dataCallback;
        config.pUserData = this;
        m_ok = ma_device_init(nullptr, &config, &m_device) == MA_SUCCESS;
        if (m_ok) {
            spdlog::info("Audio device: {} ({} Hz, {} ch, period {} frames x {})", m_device.playback.name,
                         m_device.sampleRate, m_device.playback.channels,
                         m_device.playback.internalPeriodSizeInFrames, m_device.playback.internalPeriods);
        } else {
            spdlog::warn("No audio output device available; playback will follow the system clock");
        }
    }

    ~MiniaudioDevice() override {
        if (m_ok) {
            ma_device_uninit(&m_device);
        }
    }

    MiniaudioDevice(const MiniaudioDevice&) = delete;
    MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;
    MiniaudioDevice(MiniaudioDevice&&) = delete;
    MiniaudioDevice& operator=(MiniaudioDevice&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    bool start() override { return m_ok && ma_device_start(&m_device) == MA_SUCCESS; }
    void stop() override {
        if (m_ok) {
            ma_device_stop(&m_device);
        }
    }
    [[nodiscard]] bool isRunning() const override {
        return m_ok && ma_device_is_started(const_cast<ma_device*>(&m_device)) != 0;
    }
    [[nodiscard]] int sampleRate() const override { return static_cast<int>(m_device.sampleRate); }
    [[nodiscard]] int channels() const override { return static_cast<int>(m_device.playback.channels); }
    [[nodiscard]] std::size_t latencyFrames() const override {
        return static_cast<std::size_t>(m_device.playback.internalPeriodSizeInFrames) *
               static_cast<std::size_t>(m_device.playback.internalPeriods);
    }
    [[nodiscard]] std::string name() const override { return m_device.playback.name; }

private:
    static void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
        self->m_callback(static_cast<float*>(output), frames);
    }

    RenderCallback m_callback;
    ma_device m_device{};
    bool m_ok = false;
};

} // namespace

std::unique_ptr<AudioDevice> createDefaultDevice(RenderCallback callback, int sampleRate, int channels) {
    auto device = std::make_unique<MiniaudioDevice>(std::move(callback), sampleRate, channels);
    if (!device->ok()) {
        return nullptr;
    }
    return device;
}

} // namespace loki::audio

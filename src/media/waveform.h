#pragma once

#include "util/rational.h"
#include "util/task_pool.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace loki::media {

// Min/max peak summary of an audio stream for drawing waveforms. One peak pair per channel
// every `samplesPerPeak` frames at `sampleRate`, values scaled to int16 range.
struct Waveform {
    int sampleRate = 48000;
    int channels = 0;
    int samplesPerPeak = 256;
    std::int64_t frameCount = 0;         // total audio frames summarized
    std::vector<std::int16_t> peaks;     // [peak][channel][min,max] interleaved

    [[nodiscard]] std::size_t peakCount() const noexcept {
        return channels == 0 ? 0 : peaks.size() / (static_cast<std::size_t>(channels) * 2);
    }
    [[nodiscard]] std::int16_t min(std::size_t peak, int channel) const {
        return peaks[(peak * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)) * 2];
    }
    [[nodiscard]] std::int16_t max(std::size_t peak, int channel) const {
        return peaks[(peak * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)) * 2 + 1];
    }
    [[nodiscard]] Rational duration() const { return Rational(frameCount, sampleRate); }

    friend bool operator==(const Waveform&, const Waveform&) = default;
};

// Decodes the whole audio stream and summarizes it. Polls `cancel` regularly; returns nullopt
// when cancelled. `progress` (optional) receives values in [0, 1]. Throws MediaError.
[[nodiscard]] std::optional<Waveform> computeWaveform(const std::filesystem::path& path, int samplesPerPeak = 256,
                                                      const util::CancelToken* cancel = nullptr,
                                                      const std::function<void(double)>& progress = {});

// Compact binary file format ("LKWF" v1). Throws std::runtime_error on malformed input.
void saveWaveform(const Waveform& waveform, const std::filesystem::path& file);
[[nodiscard]] Waveform loadWaveform(const std::filesystem::path& file);

} // namespace loki::media

#include "media/waveform.h"

#include "media/audio_decoder.h"
#include "util/atomic_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace loki::media {

namespace {

constexpr char kMagic[4] = {'L', 'K', 'W', 'F'};
constexpr std::uint32_t kVersion = 1;

std::int16_t toInt16(float v) {
    const float clamped = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

template <typename T>
void append(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T)); // little-endian hosts only (Windows/x64, ARM64)
    out.append(bytes, sizeof(T));
}

template <typename T>
T take(const std::string& in, std::size_t& offset) {
    if (offset + sizeof(T) > in.size()) {
        throw std::runtime_error("Truncated waveform file");
    }
    T value;
    std::memcpy(&value, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

} // namespace

std::optional<Waveform> computeWaveform(const std::filesystem::path& path, int samplesPerPeak,
                                        const util::CancelToken* cancel, const std::function<void(double)>& progress) {
    // Mono stays mono; anything wider is drawn as stereo (downmixed by swresample).
    auto decoder = std::make_unique<AudioDecoder>(path, 48000, 0);
    if (decoder->channels() > 2) {
        decoder = std::make_unique<AudioDecoder>(path, 48000, 2);
    }
    AudioDecoder& source = *decoder;
    const int channels = source.channels();

    Waveform wf;
    wf.sampleRate = 48000;
    wf.channels = channels;
    wf.samplesPerPeak = std::max(1, samplesPerPeak);
    const auto totalFrames = timeToFrames(source.info().duration, Rational(wf.sampleRate), Rounding::Ceil);

    const auto ch = static_cast<std::size_t>(channels);
    const auto block = static_cast<std::size_t>(wf.samplesPerPeak);
    std::vector<float> buffer(block * ch);
    std::size_t blocks = 0;
    while (true) {
        if (cancel != nullptr && cancel->cancelled()) {
            return std::nullopt;
        }
        const std::size_t got = source.read(buffer.data(), block);
        if (got == 0) {
            break;
        }
        for (std::size_t c = 0; c < ch; ++c) {
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            for (std::size_t i = 0; i < got; ++i) {
                const float v = buffer[i * ch + c];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            wf.peaks.push_back(toInt16(lo));
            wf.peaks.push_back(toInt16(hi));
        }
        wf.frameCount += static_cast<std::int64_t>(got);
        if (progress && totalFrames > 0 && (++blocks % 256) == 0) {
            progress(std::min(1.0, static_cast<double>(wf.frameCount) / static_cast<double>(totalFrames)));
        }
    }
    if (progress) {
        progress(1.0);
    }
    return wf;
}

void saveWaveform(const Waveform& wf, const std::filesystem::path& file) {
    std::string out(kMagic, sizeof(kMagic));
    append(out, kVersion);
    append(out, static_cast<std::int32_t>(wf.sampleRate));
    append(out, static_cast<std::int32_t>(wf.channels));
    append(out, static_cast<std::int32_t>(wf.samplesPerPeak));
    append(out, static_cast<std::int64_t>(wf.frameCount));
    append(out, static_cast<std::uint64_t>(wf.peaks.size()));
    out.append(reinterpret_cast<const char*>(wf.peaks.data()), wf.peaks.size() * sizeof(std::int16_t));
    util::writeFileAtomic(file, out);
}

Waveform loadWaveform(const std::filesystem::path& file) {
    const std::string in = util::readFile(file);
    if (in.size() < sizeof(kMagic) || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("Not a waveform file");
    }
    std::size_t offset = sizeof(kMagic);
    if (take<std::uint32_t>(in, offset) != kVersion) {
        throw std::runtime_error("Unsupported waveform file version");
    }
    Waveform wf;
    wf.sampleRate = take<std::int32_t>(in, offset);
    wf.channels = take<std::int32_t>(in, offset);
    wf.samplesPerPeak = take<std::int32_t>(in, offset);
    wf.frameCount = take<std::int64_t>(in, offset);
    const auto count = take<std::uint64_t>(in, offset);
    if (wf.sampleRate <= 0 || wf.channels <= 0 || wf.samplesPerPeak <= 0 ||
        count * sizeof(std::int16_t) != in.size() - offset) {
        throw std::runtime_error("Corrupt waveform file");
    }
    wf.peaks.resize(static_cast<std::size_t>(count));
    std::memcpy(wf.peaks.data(), in.data() + offset, wf.peaks.size() * sizeof(std::int16_t));
    return wf;
}

} // namespace loki::media

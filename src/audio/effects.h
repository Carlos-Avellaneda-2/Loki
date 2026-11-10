#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace loki::audio {

// Clip audio effects (D38). Stateful processors on interleaved float buffers; the same code runs in
// preview and export. reset() after a seek (no state crosses a discontinuity).

// --- Parametric EQ ----------------------------------------------------------------------------------

struct EqParams {
    float lowGainDb = 0.0f;   // low shelf
    float lowFreq = 100.0f;
    float midGainDb = 0.0f;   // peaking band
    float midFreq = 1000.0f;
    float midQ = 1.0f;
    float highGainDb = 0.0f;  // high shelf
    float highFreq = 8000.0f;

    [[nodiscard]] bool flat() const noexcept { return lowGainDb == 0.0f && midGainDb == 0.0f && highGainDb == 0.0f; }
    friend bool operator==(const EqParams&, const EqParams&) = default;
};

// RBJ biquad, transposed direct form II, one state per channel.
class Biquad {
public:
    enum class Kind { LowShelf, Peak, HighShelf };
    void design(Kind kind, double sampleRate, double freq, double gainDb, double q) noexcept;
    void reset() noexcept { m_z1.fill(0.0); m_z2.fill(0.0); }
    void process(float* buffer, std::size_t frames, int channels) noexcept;
    // Magnitude response at `freq` (tests).
    [[nodiscard]] double magnitudeAt(double freq, double sampleRate) const noexcept;

private:
    static constexpr std::size_t kMaxChannels = 8;
    double m_b0 = 1.0, m_b1 = 0.0, m_b2 = 0.0, m_a1 = 0.0, m_a2 = 0.0;
    std::array<double, kMaxChannels> m_z1{};
    std::array<double, kMaxChannels> m_z2{};
};

class ParametricEq {
public:
    void setParams(const EqParams& params, int sampleRate) noexcept;
    void reset() noexcept;
    void process(float* buffer, std::size_t frames, int channels) noexcept;

private:
    EqParams m_params;
    int m_rate = 0;
    bool m_designed = false;
    std::array<Biquad, 3> m_bands;
};

// --- Compressor ---------------------------------------------------------------------------------------

struct CompressorParams {
    float thresholdDb = -20.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float makeupDb = 0.0f;
};

// Feed-forward, linked stereo, peak detector smoothed in dB.
class Compressor {
public:
    void setParams(const CompressorParams& params, int sampleRate) noexcept;
    void reset() noexcept { m_envelopeDb = -120.0; }
    void process(float* buffer, std::size_t frames, int channels) noexcept;
    // Static gain reduction (dB, >= 0) for an input level (tests).
    [[nodiscard]] float gainReductionDb(float levelDb) const noexcept;

private:
    CompressorParams m_params;
    double m_attack = 0.0;
    double m_release = 0.0;
    double m_envelopeDb = -120.0;
};

// --- Noise reduction ------------------------------------------------------------------------------------

// In-place radix-2 FFT (size a power of two). `inverse` divides by the size.
void fft(std::vector<std::complex<float>>& data, bool inverse);

// Spectral subtraction with a minimum-tracking noise estimate per bin (STFT 1024, hop 256, Hann).
// The output is delayed by latency() frames.
class NoiseReducer {
public:
    static constexpr std::size_t kFrame = 1024;
    static constexpr std::size_t kHop = 256;

    void setParams(float reductionDb, int channels);
    void reset();
    void process(float* buffer, std::size_t frames, int channels);
    [[nodiscard]] static constexpr std::size_t latency() noexcept { return kFrame; }

private:
    struct Channel {
        std::vector<float> input;  // last kFrame input samples
        std::vector<float> output; // overlap-add accumulator (kFrame)
        std::vector<float> smooth; // power per bin smoothed over time
        std::vector<float> noise;  // noise power per bin (minimum of the smoothed power, slowly rising)
        std::vector<float> gain;   // smoothed gain per bin
        bool primed = false;
        int frames = 0; // analysed frames since reset (the estimate learns quickly at first)
    };
    void processFrame(Channel& ch);

    float m_floor = 0.25f; // minimum gain (from the reduction amount)
    int m_channels = 0;
    std::vector<Channel> m_state;
    std::vector<float> m_pending; // interleaved input not yet hop-aligned
    std::vector<float> m_ready;   // interleaved output ready to hand out
    std::size_t m_fill = 0;       // samples per channel since the last hop
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_spectrum;
};

// --- Time stretch (Maintain Audio Pitch) ----------------------------------------------------------------

// WSOLA: output grains of 1024 frames every 512, each taken near its nominal source position where
// it best continues the previous one, so speed changes keep the pitch.
class TimeStretcher {
public:
    static constexpr std::int64_t kGrain = 1024;
    static constexpr std::int64_t kHop = 512;
    static constexpr std::int64_t kTolerance = 256;

    // Source access: fills `frames` interleaved frames from integer source frame `pos` (zeros outside).
    using Fetch = std::function<void(std::int64_t pos, std::size_t frames, float* out)>;
    // Nominal source frame of an output frame.
    using SourceAt = std::function<double(std::int64_t outputFrame)>;

    void reset();
    // Writes output frames [outStart, outStart + count) to `out` (interleaved).
    void render(std::int64_t outStart, std::size_t count, int channels, const SourceAt& sourceAt, const Fetch& fetch,
                float* out);

private:
    void produceChunk(std::int64_t chunkStart, int channels, const SourceAt& sourceAt, const Fetch& fetch);

    bool m_started = false;
    std::int64_t m_nextChunk = 0;  // output frame where the next chunk starts
    std::int64_t m_prevPos = 0;    // source frame of the previous grain
    std::vector<float> m_tail;     // second half of the previous grain (kHop frames)
    std::vector<float> m_produced; // produced output frames from m_producedStart
    std::int64_t m_producedStart = 0;
    std::vector<float> m_grain;
    std::vector<float> m_window;
};

} // namespace loki::audio

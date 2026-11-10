#include "audio/effects.h"

#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace loki::audio {

// --- Biquad / EQ -----------------------------------------------------------------------------------

void Biquad::design(Kind kind, double rate, double freq, double gainDb, double q) noexcept {
    freq = std::clamp(freq, 10.0, rate * 0.45);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * freq / rate;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    if (kind == Kind::Peak) {
        const double alpha = sw / (2.0 * std::max(q, 0.05));
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha / a;
    } else {
        const double alpha = sw / 2.0 * std::sqrt(2.0); // shelf slope S = 1
        const double sqa = 2.0 * std::sqrt(a) * alpha;
        if (kind == Kind::LowShelf) {
            b0 = a * ((a + 1) - (a - 1) * cw + sqa);
            b1 = 2 * a * ((a - 1) - (a + 1) * cw);
            b2 = a * ((a + 1) - (a - 1) * cw - sqa);
            a0 = (a + 1) + (a - 1) * cw + sqa;
            a1 = -2 * ((a - 1) + (a + 1) * cw);
            a2 = (a + 1) + (a - 1) * cw - sqa;
        } else {
            b0 = a * ((a + 1) + (a - 1) * cw + sqa);
            b1 = -2 * a * ((a - 1) + (a + 1) * cw);
            b2 = a * ((a + 1) + (a - 1) * cw - sqa);
            a0 = (a + 1) - (a - 1) * cw + sqa;
            a1 = 2 * ((a - 1) - (a + 1) * cw);
            a2 = (a + 1) - (a - 1) * cw - sqa;
        }
    }
    m_b0 = b0 / a0;
    m_b1 = b1 / a0;
    m_b2 = b2 / a0;
    m_a1 = a1 / a0;
    m_a2 = a2 / a0;
}

void Biquad::process(float* buffer, std::size_t frames, int channels) noexcept {
    const auto ch = static_cast<std::size_t>(std::min<int>(channels, static_cast<int>(kMaxChannels)));
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < ch; ++c) {
            auto& s = buffer[i * static_cast<std::size_t>(channels) + c];
            const double x = s;
            const double y = m_b0 * x + m_z1[c];
            m_z1[c] = m_b1 * x - m_a1 * y + m_z2[c];
            m_z2[c] = m_b2 * x - m_a2 * y;
            s = static_cast<float>(y);
        }
    }
}

double Biquad::magnitudeAt(double freq, double rate) const noexcept {
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * freq / rate);
    const auto num = m_b0 + m_b1 * z + m_b2 * z * z;
    const auto den = 1.0 + m_a1 * z + m_a2 * z * z;
    return std::abs(num / den);
}

void ParametricEq::setParams(const EqParams& p, int rate) noexcept {
    if (m_designed && p == m_params && rate == m_rate) {
        return;
    }
    m_params = p;
    m_rate = rate;
    m_designed = true;
    m_bands[0].design(Biquad::Kind::LowShelf, rate, p.lowFreq, p.lowGainDb, 0.707);
    m_bands[1].design(Biquad::Kind::Peak, rate, p.midFreq, p.midGainDb, p.midQ);
    m_bands[2].design(Biquad::Kind::HighShelf, rate, p.highFreq, p.highGainDb, 0.707);
}

void ParametricEq::reset() noexcept {
    for (auto& b : m_bands) {
        b.reset();
    }
}

void ParametricEq::process(float* buffer, std::size_t frames, int channels) noexcept {
    if (m_params.flat()) {
        return;
    }
    for (auto& b : m_bands) {
        b.process(buffer, frames, channels);
    }
}

// --- Compressor ---------------------------------------------------------------------------------------

void Compressor::setParams(const CompressorParams& p, int rate) noexcept {
    m_params = p;
    m_params.ratio = std::max(1.0f, p.ratio);
    m_attack = std::exp(-1.0 / (std::max(0.1f, p.attackMs) * 0.001 * rate));
    m_release = std::exp(-1.0 / (std::max(1.0f, p.releaseMs) * 0.001 * rate));
}

float Compressor::gainReductionDb(float levelDb) const noexcept {
    const float over = levelDb - m_params.thresholdDb;
    return over > 0.0f ? over * (1.0f - 1.0f / m_params.ratio) : 0.0f;
}

void Compressor::process(float* buffer, std::size_t frames, int channels) noexcept {
    const auto ch = static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (std::size_t c = 0; c < ch; ++c) {
            peak = std::max(peak, std::abs(buffer[i * ch + c]));
        }
        const double levelDb = peak > 1e-6f ? 20.0 * std::log10(peak) : -120.0;
        const double coeff = levelDb > m_envelopeDb ? m_attack : m_release;
        m_envelopeDb = levelDb + coeff * (m_envelopeDb - levelDb);
        const float gain = dbToGain(m_params.makeupDb - gainReductionDb(static_cast<float>(m_envelopeDb)));
        for (std::size_t c = 0; c < ch; ++c) {
            buffer[i * ch + c] *= gain;
        }
    }
}

// --- FFT / noise reduction ------------------------------------------------------------------------------

void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * std::numbers::pi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<float> wl(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) {
        for (auto& x : a) {
            x /= static_cast<float>(n);
        }
    }
}

void NoiseReducer::setParams(float reductionDb, int channels) {
    m_floor = dbToGain(-std::clamp(reductionDb, 0.0f, 60.0f));
    if (channels != m_channels) {
        m_channels = channels;
        reset();
    }
}

void NoiseReducer::reset() {
    m_window.resize(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
        m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / kFrame));
    }
    m_spectrum.assign(kFrame, {});
    m_state.assign(static_cast<std::size_t>(std::max(0, m_channels)), {});
    for (auto& c : m_state) {
        c.input.assign(kFrame, 0.0f);
        c.output.assign(kFrame, 0.0f);
        c.noise.assign(kFrame / 2 + 1, 0.0f);
        c.smooth.assign(kFrame / 2 + 1, 0.0f);
        c.gain.assign(kFrame / 2 + 1, 1.0f);
    }
    m_fill = 0;
    // One hop of zeros so every call can hand out as many frames as it receives.
    m_ready.assign(kHop * static_cast<std::size_t>(std::max(0, m_channels)), 0.0f);
}

void NoiseReducer::processFrame(Channel& ch) {
    for (std::size_t i = 0; i < kFrame; ++i) {
        m_spectrum[i] = {ch.input[i] * m_window[i], 0.0f};
    }
    fft(m_spectrum, false);
    for (std::size_t k = 0; k <= kFrame / 2; ++k) {
        const float power = std::norm(m_spectrum[k]) + 1e-12f;
        ch.smooth[k] = ch.primed ? 0.85f * ch.smooth[k] + 0.15f * power : power;
        // First ~0.5 s: follow the smoothed power (the window starts on silence); then track its minimum.
        if (!ch.primed || ch.frames < 96 || ch.smooth[k] < ch.noise[k]) {
            ch.noise[k] = ch.smooth[k];
        } else {
            ch.noise[k] *= 1.0025f; // slow rise: follows a changing noise floor
        }
        const float g = std::max(m_floor, 1.0f - 2.0f * ch.noise[k] / power);
        ch.gain[k] = 0.5f * ch.gain[k] + 0.5f * g;
        m_spectrum[k] *= ch.gain[k];
        if (k > 0 && k < kFrame / 2) {
            m_spectrum[kFrame - k] = std::conj(m_spectrum[k]);
        }
    }
    ch.primed = true;
    ++ch.frames;
    fft(m_spectrum, true);
    // Hann analysis + synthesis at 75 % overlap sums to 1.5.
    for (std::size_t i = 0; i < kFrame; ++i) {
        ch.output[i] += m_spectrum[i].real() * m_window[i] * (2.0f / 3.0f);
    }
}

void NoiseReducer::process(float* buffer, std::size_t frames, int channels) {
    if (channels != m_channels || m_state.empty()) {
        m_channels = channels;
        reset();
    }
    const auto chs = static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < chs; ++c) {
            auto& st = m_state[c];
            st.input[kFrame - kHop + m_fill] = buffer[i * chs + c];
        }
        if (++m_fill == kHop) {
            m_fill = 0;
            for (std::size_t c = 0; c < chs; ++c) {
                auto& st = m_state[c];
                processFrame(st);
                std::move(st.input.begin() + kHop, st.input.end(), st.input.begin());
            }
            const std::size_t base = m_ready.size();
            m_ready.resize(base + kHop * chs);
            for (std::size_t c = 0; c < chs; ++c) {
                auto& st = m_state[c];
                for (std::size_t k = 0; k < kHop; ++k) {
                    m_ready[base + k * chs + c] = st.output[k];
                }
                std::move(st.output.begin() + kHop, st.output.end(), st.output.begin());
                std::fill(st.output.end() - kHop, st.output.end(), 0.0f);
            }
        }
    }
    const std::size_t take = std::min(frames * chs, m_ready.size());
    std::copy(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(take), buffer);
    std::fill(buffer + take, buffer + frames * chs, 0.0f);
    m_ready.erase(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(take));
}

// --- WSOLA ------------------------------------------------------------------------------------------------

void TimeStretcher::reset() {
    m_started = false;
    m_tail.clear();
    m_produced.clear();
}

void TimeStretcher::produceChunk(std::int64_t chunkStart, int channels, const SourceAt& sourceAt, const Fetch& fetch) {
    const auto ch = static_cast<std::size_t>(channels);
    if (m_window.size() != static_cast<std::size_t>(kGrain)) {
        m_window.resize(static_cast<std::size_t>(kGrain));
        for (std::size_t i = 0; i < m_window.size(); ++i) {
            m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / kGrain));
        }
    }
    auto pos = static_cast<std::int64_t>(std::llround(sourceAt(chunkStart)));
    if (!m_tail.empty()) {
        // Best continuation: the grain start whose first hop looks most like the natural continuation
        // of the previous grain (mono cross-correlation, coarse then fine).
        const std::int64_t natural = m_prevPos + kHop;
        std::vector<float> ref(static_cast<std::size_t>(kHop) * ch);
        fetch(natural, static_cast<std::size_t>(kHop), ref.data());
        std::vector<float> area(static_cast<std::size_t>(kHop + 2 * kTolerance) * ch);
        fetch(pos - kTolerance, static_cast<std::size_t>(kHop + 2 * kTolerance), area.data());
        const auto mono = [&](const std::vector<float>& v, std::int64_t i) {
            float s = 0.0f;
            for (std::size_t c = 0; c < ch; ++c) {
                s += v[static_cast<std::size_t>(i) * ch + c];
            }
            return s;
        };
        const auto score = [&](std::int64_t offset) {
            double sum = 0.0;
            for (std::int64_t i = 0; i < kHop; i += 2) {
                sum += static_cast<double>(mono(ref, i)) * mono(area, i + offset);
            }
            return sum;
        };
        std::int64_t best = kTolerance;
        double bestScore = score(best);
        for (std::int64_t o = 0; o <= 2 * kTolerance; o += 4) {
            const double s = score(o);
            if (s > bestScore) {
                bestScore = s;
                best = o;
            }
        }
        for (std::int64_t o = std::max<std::int64_t>(0, best - 3); o <= std::min<std::int64_t>(2 * kTolerance, best + 3); ++o) {
            const double s = score(o);
            if (s > bestScore) {
                bestScore = s;
                best = o;
            }
        }
        pos += best - kTolerance;
    }
    m_grain.assign(static_cast<std::size_t>(kGrain) * ch, 0.0f);
    fetch(pos, static_cast<std::size_t>(kGrain), m_grain.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(kGrain); ++i) {
        for (std::size_t c = 0; c < ch; ++c) {
            m_grain[i * ch + c] *= m_window[i];
        }
    }
    if (m_tail.size() != static_cast<std::size_t>(kHop) * ch) {
        m_tail.assign(static_cast<std::size_t>(kHop) * ch, 0.0f);
    }
    const std::size_t base = m_produced.size();
    m_produced.resize(base + static_cast<std::size_t>(kHop) * ch);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kHop) * ch; ++i) {
        m_produced[base + i] = m_tail[i] + m_grain[i];
    }
    std::copy(m_grain.begin() + static_cast<std::ptrdiff_t>(kHop * static_cast<std::int64_t>(ch)), m_grain.end(), m_tail.begin());
    m_prevPos = pos;
    m_nextChunk = chunkStart + kHop;
}

void TimeStretcher::render(std::int64_t outStart, std::size_t count, int channels, const SourceAt& sourceAt,
                           const Fetch& fetch, float* out) {
    const auto ch = static_cast<std::size_t>(channels);
    const auto producedEnd = m_producedStart + static_cast<std::int64_t>(m_produced.size() / std::max<std::size_t>(ch, 1));
    if (!m_started || outStart < m_producedStart || outStart > producedEnd + kHop) {
        // Restart on the hop grid, one warm-up chunk before (so the first output has a full overlap).
        const std::int64_t first = (outStart >= 0 ? outStart / kHop : (outStart - kHop + 1) / kHop) * kHop;
        m_tail.clear();
        m_produced.clear();
        produceChunk(first - kHop, channels, sourceAt, fetch);
        m_produced.clear();
        m_producedStart = first;
        m_started = true;
    }
    const std::int64_t end = outStart + static_cast<std::int64_t>(count);
    while (m_nextChunk < end) {
        produceChunk(m_nextChunk, channels, sourceAt, fetch);
    }
    const auto from = static_cast<std::size_t>(outStart - m_producedStart) * ch;
    std::copy(m_produced.begin() + static_cast<std::ptrdiff_t>(from),
              m_produced.begin() + static_cast<std::ptrdiff_t>(from + count * ch), out);
    // Keep only what a following call may still need.
    const auto drop = static_cast<std::size_t>(end - m_producedStart) * ch;
    m_produced.erase(m_produced.begin(), m_produced.begin() + static_cast<std::ptrdiff_t>(std::min(drop, m_produced.size())));
    m_producedStart = end;
}

} // namespace loki::audio

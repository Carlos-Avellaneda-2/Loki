#include "util/timecode.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace loki {

namespace {

// Frames skipped at the start of each minute (except every tenth) in drop-frame timecode.
std::int64_t dropFramesPerMinute(std::int64_t fps) noexcept {
    return fps / 15; // 30 -> 2, 60 -> 4
}

std::optional<std::int64_t> parseField(std::string_view s) {
    if (s.empty() || s.size() > 9) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc() || ptr != end || value < 0) {
        return std::nullopt;
    }
    return value;
}

} // namespace

bool supportsDropFrame(const Rational& frameRate) noexcept {
    return frameRate == Rational(30000, 1001) || frameRate == Rational(60000, 1001);
}

std::int64_t nominalFps(const Rational& frameRate) {
    if (!frameRate.isPositive()) {
        throw std::domain_error("Frame rate must be positive");
    }
    const auto fps = frameRate.round();
    return fps < 1 ? 1 : fps;
}

std::string formatTimecode(std::int64_t frame, const Rational& frameRate, bool dropFrame) {
    const std::int64_t fps = nominalFps(frameRate);
    const bool df = dropFrame && supportsDropFrame(frameRate);
    const bool negative = frame < 0;
    std::uint64_t f = negative ? static_cast<std::uint64_t>(-(frame + 1)) + 1 : static_cast<std::uint64_t>(frame);

    if (df) {
        const auto d = static_cast<std::uint64_t>(dropFramesPerMinute(fps));
        const auto framesPerMinute = static_cast<std::uint64_t>(fps) * 60 - d;
        const auto framesPer10Minutes = framesPerMinute * 10 + d;
        const auto tens = f / framesPer10Minutes;
        const auto rem = f % framesPer10Minutes;
        f += d * 9 * tens;
        if (rem > d) {
            f += d * ((rem - d) / framesPerMinute);
        }
    }

    const auto ufps = static_cast<std::uint64_t>(fps);
    const auto ff = f % ufps;
    const auto totalSeconds = f / ufps;
    const auto ss = totalSeconds % 60;
    const auto mm = (totalSeconds / 60) % 60;
    const auto hh = totalSeconds / 3600;

    const int frameDigits = fps > 100 ? 3 : 2;
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s%02llu:%02llu:%02llu%c%0*llu", negative ? "-" : "",
                  static_cast<unsigned long long>(hh), static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss), df ? ';' : ':', frameDigits, static_cast<unsigned long long>(ff));
    return buffer.data();
}

std::optional<std::int64_t> parseTimecode(std::string_view text, const Rational& frameRate, bool dropFrame) {
    if (!frameRate.isPositive()) {
        return std::nullopt;
    }
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }

    std::vector<std::string_view> parts;
    bool sawDropSeparator = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ':' || text[i] == ';' || text[i] == '.') {
            if (i < text.size() && text[i] == ';') {
                sawDropSeparator = true;
            }
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty() || parts.size() > 4) {
        return std::nullopt;
    }

    std::array<std::int64_t, 4> fields{}; // hh, mm, ss, ff (right-aligned)
    const std::size_t offset = 4 - parts.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto v = parseField(parts[i]);
        if (!v) {
            return std::nullopt;
        }
        fields[offset + i] = *v;
    }

    const std::int64_t fps = nominalFps(frameRate);
    if (parts.size() == 1) {
        return negative ? -fields[3] : fields[3]; // bare frame count
    }

    const auto [hh, mm, ss, ff] = fields;
    if (mm > 59 || ss > 59 || ff >= fps) {
        return std::nullopt;
    }

    const bool df = (dropFrame || sawDropSeparator) && supportsDropFrame(frameRate);
    std::int64_t frame = ((hh * 60 + mm) * 60 + ss) * fps + ff;
    if (df) {
        const std::int64_t d = dropFramesPerMinute(fps);
        if (ss == 0 && ff < d && mm % 10 != 0) {
            return std::nullopt; // label skipped by drop-frame
        }
        const std::int64_t totalMinutes = hh * 60 + mm;
        frame -= d * (totalMinutes - totalMinutes / 10);
    }
    return negative ? -frame : frame;
}

} // namespace loki

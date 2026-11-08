#pragma once

#include "util/rational.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace loki {

// SMPTE timecode helpers. Timecode is presentation only: the model stores Rational times.

// True for 30000/1001 and 60000/1001, the rates where drop-frame timecode is defined.
[[nodiscard]] bool supportsDropFrame(const Rational& frameRate) noexcept;

// Integer frames-per-second label used by timecode (e.g. 29.97 -> 30, 23.976 -> 24).
[[nodiscard]] std::int64_t nominalFps(const Rational& frameRate);

// Formats a frame index as HH:MM:SS:FF (non-drop) or HH:MM:SS;FF (drop-frame).
// Negative frames are prefixed with '-'. dropFrame is ignored for rates that do not support it.
[[nodiscard]] std::string formatTimecode(std::int64_t frame, const Rational& frameRate, bool dropFrame = false);

// Parses HH:MM:SS:FF / HH:MM:SS;FF. Shorter forms are right-aligned (SS:FF, MM:SS:FF), a bare
// number is a frame count. Returns nullopt for malformed text, out-of-range fields, or frame
// labels that drop-frame skips.
[[nodiscard]] std::optional<std::int64_t> parseTimecode(std::string_view text, const Rational& frameRate,
                                                        bool dropFrame = false);

} // namespace loki

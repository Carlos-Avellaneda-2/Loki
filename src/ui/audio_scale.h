#pragma once

#include "audio/mixer.h"

#include <algorithm>

namespace loki::ui {

// Fader-style dB scale shared by the timeline volume line, the mixer faders and the meters:
// 0..0.4 covers -60..-20 dB, 0.4..1 covers -20 dB..maxDb. Positions <= 0 are silence.
inline constexpr double kScaleLowDb = -60.0;
inline constexpr double kScaleKneeDb = -20.0;
inline constexpr double kScaleKneePos = 0.4;

[[nodiscard]] inline double dbToScale(double db, double maxDb) noexcept {
    if (db <= kScaleLowDb) {
        return 0.0;
    }
    if (db < kScaleKneeDb) {
        return kScaleKneePos * (db - kScaleLowDb) / (kScaleKneeDb - kScaleLowDb);
    }
    return std::min(1.0, kScaleKneePos + (1.0 - kScaleKneePos) * (db - kScaleKneeDb) / (maxDb - kScaleKneeDb));
}

[[nodiscard]] inline double scaleToDb(double pos, double maxDb) noexcept {
    if (pos <= 0.0) {
        return audio::kSilenceDb;
    }
    if (pos < kScaleKneePos) {
        return kScaleLowDb + (kScaleKneeDb - kScaleLowDb) * pos / kScaleKneePos;
    }
    return kScaleKneeDb + (maxDb - kScaleKneeDb) * (std::min(pos, 1.0) - kScaleKneePos) / (1.0 - kScaleKneePos);
}

} // namespace loki::ui

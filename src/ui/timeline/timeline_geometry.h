#pragma once

#include "model/types.h"

#include <optional>
#include <set>
#include <vector>

namespace loki::ui::timeline {

// Presentation-only geometry for the timeline. Doubles are allowed here (pixels); every value
// that goes back into the model is converted to a frame-snapped Rational first.
struct Metrics {
    double pixelsPerSecond = 60.0;
    double scrollX = 0.0; // pixels scrolled from time 0
    int rulerHeight = 30;
    int trackHeight = 52;
    int dividerHeight = 8; // between video and audio tracks
    int scrollY = 0;
};

[[nodiscard]] double timeToX(const Rational& t, const Metrics& m) noexcept;
// Time at x, snapped to the sequence frame grid (never negative).
[[nodiscard]] Rational xToTime(double x, const Metrics& m, const Rational& frameRate);

struct TrackRow {
    model::TrackId id;
    model::TrackKind kind = model::TrackKind::Video;
    std::size_t index = 0; // index in the sequence's V or A list
    int top = 0;
    int height = 0;
    [[nodiscard]] bool contains(int y) const noexcept { return y >= top && y < top + height; }
};

// Premiere layout: video tracks top-down from the highest (Vn ... V1), a divider, then audio
// tracks A1 ... An. Rows start below the ruler and move with scrollY.
[[nodiscard]] std::vector<TrackRow> layoutTracks(const model::Sequence& sequence, const Metrics& m);
[[nodiscard]] const TrackRow* rowAt(const std::vector<TrackRow>& rows, int y) noexcept;
[[nodiscard]] int contentHeight(const model::Sequence& sequence, const Metrics& m) noexcept;

enum class HitZone { None, Body, InEdge, OutEdge };

struct Hit {
    model::ClipId clip;
    model::TrackId track;
    HitZone zone = HitZone::None;
};

// Clip under (x, y). Edges are `edgePx` wide inside the clip (narrow clips favour the body).
[[nodiscard]] Hit hitTest(const model::Sequence& sequence, const std::vector<TrackRow>& rows, const Metrics& m,
                          double x, int y, double edgePx = 6.0);

// Snap targets: clip edges (except excluded clips), the playhead and the sequence start.
[[nodiscard]] std::vector<Rational> snapCandidates(const model::Sequence& sequence, const Rational& playhead,
                                                   const std::set<model::ClipId>& exclude = {});
// Closest candidate within `tolerancePx` of `t`, if any.
[[nodiscard]] std::optional<Rational> snapTime(const Rational& t, const std::vector<Rational>& candidates,
                                               const Metrics& m, double tolerancePx = 8.0);
// For a moving block [start, end): the delta (added to the proposed delta) that snaps either
// edge to a candidate, if any is within tolerance.
[[nodiscard]] std::optional<Rational> snapBlock(const Rational& start, const Rational& end,
                                                const std::vector<Rational>& candidates, const Metrics& m,
                                                double tolerancePx = 8.0);

// Ruler tick spacing (seconds between labelled ticks) so labels are at least `minPx` apart.
[[nodiscard]] Rational rulerStep(const Metrics& m, const Rational& frameRate, double minPx = 90.0);

// Previous/next edit point (clip boundary) strictly before/after `t` on any track.
[[nodiscard]] std::optional<Rational> previousEditPoint(const model::Sequence& sequence, const Rational& t);
[[nodiscard]] std::optional<Rational> nextEditPoint(const model::Sequence& sequence, const Rational& t);

} // namespace loki::ui::timeline

#include "ui/timeline/timeline_geometry.h"

#include <algorithm>
#include <cmath>

namespace loki::ui::timeline {

double timeToX(const Rational& t, const Metrics& m) noexcept {
    return t.toDouble() * m.pixelsPerSecond - m.scrollX;
}

Rational xToTime(double x, const Metrics& m, const Rational& frameRate) {
    const double seconds = std::max(0.0, (x + m.scrollX) / m.pixelsPerSecond);
    // Frame index computed in double only for the pixel->frame step, then exact from there on.
    const auto frame = static_cast<std::int64_t>(std::floor(seconds * frameRate.toDouble() + 1e-9));
    return framesToTime(std::max<std::int64_t>(0, frame), frameRate);
}

std::vector<TrackRow> layoutTracks(const model::Sequence& sequence, const Metrics& m) {
    std::vector<TrackRow> rows;
    int y = m.rulerHeight - m.scrollY;
    for (std::size_t i = sequence.videoTracks.size(); i-- > 0;) {
        rows.push_back({sequence.videoTracks[i].id, model::TrackKind::Video, i, y, m.trackHeight});
        y += m.trackHeight;
    }
    y += m.dividerHeight;
    for (std::size_t i = 0; i < sequence.audioTracks.size(); ++i) {
        rows.push_back({sequence.audioTracks[i].id, model::TrackKind::Audio, i, y, m.trackHeight});
        y += m.trackHeight;
    }
    return rows;
}

const TrackRow* rowAt(const std::vector<TrackRow>& rows, int y) noexcept {
    for (const auto& r : rows) {
        if (r.contains(y)) {
            return &r;
        }
    }
    return nullptr;
}

int contentHeight(const model::Sequence& sequence, const Metrics& m) noexcept {
    return m.rulerHeight + m.dividerHeight +
           static_cast<int>(sequence.videoTracks.size() + sequence.audioTracks.size()) * m.trackHeight;
}

Hit hitTest(const model::Sequence& sequence, const std::vector<TrackRow>& rows, const Metrics& m, double x, int y,
            double edgePx) {
    const auto* row = rowAt(rows, y);
    if (row == nullptr) {
        return {};
    }
    const auto& tracks = sequence.tracks(row->kind);
    const auto& track = tracks[row->index];
    for (const auto& c : track.clips) {
        const double x0 = timeToX(c.start, m);
        const double x1 = timeToX(c.end(), m);
        if (x < x0 || x >= x1) {
            continue;
        }
        const double edge = std::min(edgePx, (x1 - x0) / 4.0);
        HitZone zone = HitZone::Body;
        if (x < x0 + edge) {
            zone = HitZone::InEdge;
        } else if (x >= x1 - edge) {
            zone = HitZone::OutEdge;
        }
        return {c.id, track.id, zone};
    }
    return {model::ClipId(), track.id, HitZone::None};
}

std::vector<Rational> snapCandidates(const model::Sequence& sequence, const Rational& playhead,
                                     const std::set<model::ClipId>& exclude) {
    std::vector<Rational> out{Rational(), playhead};
    for (const auto* list : {&sequence.videoTracks, &sequence.audioTracks}) {
        for (const auto& t : *list) {
            for (const auto& c : t.clips) {
                if (!exclude.contains(c.id)) {
                    out.push_back(c.start);
                    out.push_back(c.end());
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<Rational> snapTime(const Rational& t, const std::vector<Rational>& candidates, const Metrics& m,
                                 double tolerancePx) {
    std::optional<Rational> best;
    double bestDistance = tolerancePx;
    for (const auto& c : candidates) {
        const double d = std::abs((c - t).toDouble()) * m.pixelsPerSecond;
        if (d <= bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

std::optional<Rational> snapBlock(const Rational& start, const Rational& end, const std::vector<Rational>& candidates,
                                  const Metrics& m, double tolerancePx) {
    std::optional<Rational> best;
    double bestDistance = tolerancePx;
    for (const auto& edge : {start, end}) {
        for (const auto& c : candidates) {
            const double d = std::abs((c - edge).toDouble()) * m.pixelsPerSecond;
            if (d <= bestDistance) {
                bestDistance = d;
                best = c - edge;
            }
        }
    }
    return best;
}

Rational rulerStep(const Metrics& m, const Rational& frameRate, double minPx) {
    // Candidate steps: frames (1, 2, 5, 10), then seconds and minutes.
    const Rational frame = frameRate.reciprocal();
    const Rational steps[] = {frame,          frame * Rational(2), frame * Rational(5), frame * Rational(10),
                              Rational(1),    Rational(2),         Rational(5),         Rational(10),
                              Rational(15),   Rational(30),        Rational(60),        Rational(120),
                              Rational(300),  Rational(600),       Rational(1800),      Rational(3600)};
    for (const auto& s : steps) {
        if (s.toDouble() * m.pixelsPerSecond >= minPx) {
            return s;
        }
    }
    return Rational(3600);
}

std::optional<Rational> previousEditPoint(const model::Sequence& sequence, const Rational& t) {
    std::optional<Rational> best;
    for (const auto& c : snapCandidates(sequence, Rational())) {
        if (c < t) {
            best = c;
        }
    }
    return best;
}

std::optional<Rational> nextEditPoint(const model::Sequence& sequence, const Rational& t) {
    for (const auto& c : snapCandidates(sequence, Rational())) {
        if (c > t) {
            return c;
        }
    }
    return std::nullopt;
}

} // namespace loki::ui::timeline

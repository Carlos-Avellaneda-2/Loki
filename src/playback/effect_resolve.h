#pragma once

#include "model/types.h"
#include "render/composite.h"

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace loki::playback {

// A mask path flattened to a polygon (bezier segments subdivided), scaled by (sx, sy).
[[nodiscard]] std::vector<render::MaskPoint> flattenMaskPath(const std::vector<model::MaskVertex>& path, double sx,
                                                             double sy);

// Lumetri curves of an effect as a lookup table (null when every curve is the identity).
[[nodiscard]] std::shared_ptr<const render::CurveTable> curveTableOf(const model::Effect& effect);

// "#rrggbb" -> 0..1 RGB (fallback when malformed).
[[nodiscard]] render::Rgb parseHexColor(const std::string& text, render::Rgb fallback);

// Resolves the Phase 14 effects of a clip into a render::Layer (Lumetri, Ultra Key, blur/sharpen,
// masks, stabilizer) with caches for what does not change from frame to frame (curve tables, static
// masks, stabilization plans). Used by the playback thread; not thread-safe.
class EffectResolver {
public:
    // `layer` must have its picture, source size and grade already set.
    void apply(const model::Clip& clip, const model::MediaInfo& media, const model::SequenceSettings& settings,
               const Rational& sourceTime, render::Layer& layer);

private:
    render::MaskImagePtr masksFor(const model::Effect& effect, const Rational& sourceTime, const render::Layer& layer);

    struct CachedMask {
        std::string key;
        render::MaskImagePtr image;
    };
    std::list<CachedMask> m_masks; // LRU
    std::unordered_map<std::string, std::shared_ptr<const render::CurveTable>> m_curves;
    struct CachedPlan {
        std::string key;
        render::StabilizerAnalysis analysis;
        render::StabilizationPlan plan;
    };
    std::unordered_map<model::EffectId, CachedPlan> m_plans;
};

} // namespace loki::playback

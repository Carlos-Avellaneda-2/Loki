#include "playback/effect_resolve.h"

#include "model/effect_registry.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace loki::playback {

namespace {

constexpr std::size_t kMaskCacheSize = 48;
constexpr int kMaskMaxSide = 512; // mask resolution bound (masks are soft, GL samples them bilinearly)

double value(const model::Clip& clip, std::string_view type, std::string_view key, const Rational& t,
             const model::MediaInfo& media, const model::SequenceSettings& settings) {
    return model::effectiveValue(clip, type, key, t, media, settings);
}

const model::Effect* enabledEffect(const model::Clip& clip, std::string_view type) {
    const auto* e = model::findEffectByType(clip, type);
    return e != nullptr && e->enabled ? e : nullptr;
}

} // namespace

std::vector<render::MaskPoint> flattenMaskPath(const std::vector<model::MaskVertex>& path, double sx, double sy) {
    std::vector<render::MaskPoint> out;
    const std::size_t n = path.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = path[i];
        const auto& b = path[(i + 1) % n];
        const bool straight = a.outX == 0.0 && a.outY == 0.0 && b.inX == 0.0 && b.inY == 0.0;
        const int steps = straight ? 1 : 16;
        for (int s = 0; s < steps; ++s) {
            const double t = static_cast<double>(s) / steps;
            const double u = 1.0 - t;
            const double c0 = u * u * u;
            const double c1 = 3 * u * u * t;
            const double c2 = 3 * u * t * t;
            const double c3 = t * t * t;
            const double x = c0 * a.x + c1 * (a.x + a.outX) + c2 * (b.x + b.inX) + c3 * b.x;
            const double y = c0 * a.y + c1 * (a.y + a.outY) + c2 * (b.y + b.inY) + c3 * b.y;
            out.push_back({x * sx, y * sy});
        }
    }
    return out;
}

std::shared_ptr<const render::CurveTable> curveTableOf(const model::Effect& effect) {
    const auto curve = [&](std::string_view name) -> const std::vector<model::CurvePoint>* {
        const auto it = effect.curves.find(std::string(name));
        return it == effect.curves.end() || it->second == model::identityCurve() ? nullptr : &it->second;
    };
    const auto* master = curve(model::kCurveMaster);
    const std::vector<model::CurvePoint>* channels[3] = {curve(model::kCurveRed), curve(model::kCurveGreen),
                                                         curve(model::kCurveBlue)};
    if (master == nullptr && channels[0] == nullptr && channels[1] == nullptr && channels[2] == nullptr) {
        return nullptr;
    }
    auto table = std::make_shared<render::CurveTable>();
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t i = 0; i < 256; ++i) {
            double v = static_cast<double>(i) / 255.0;
            if (master != nullptr) {
                v = model::evaluateCurve(*master, v);
            }
            if (channels[c] != nullptr) {
                v = model::evaluateCurve(*channels[c], v);
            }
            table->rgb[c][i] = static_cast<float>(v);
        }
    }
    return table;
}

render::Rgb parseHexColor(const std::string& text, render::Rgb fallback) {
    if (text.size() != 7 || text[0] != '#') {
        return fallback;
    }
    render::Rgb out{};
    for (std::size_t i = 0; i < 3; ++i) {
        int v = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const char ch = text[1 + i * 2 + k];
            int d = 0;
            if (ch >= '0' && ch <= '9') {
                d = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                d = ch - 'a' + 10;
            } else if (ch >= 'A' && ch <= 'F') {
                d = ch - 'A' + 10;
            } else {
                return fallback;
            }
            v = v * 16 + d;
        }
        out[i] = static_cast<float>(v) / 255.0f;
    }
    return out;
}

render::MaskImagePtr EffectResolver::masksFor(const model::Effect& effect, const Rational& t, const render::Layer& layer) {
    if (effect.masks.empty() || layer.sourceWidth <= 0 || layer.sourceHeight <= 0) {
        return nullptr;
    }
    const double scale = std::min(1.0, kMaskMaxSide / std::max(layer.sourceWidth, layer.sourceHeight));
    const int w = std::max(1, static_cast<int>(std::lround(layer.sourceWidth * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(layer.sourceHeight * scale)));
    std::vector<render::ResolvedMask> resolved;
    std::ostringstream key;
    key.precision(9);
    key << w << 'x' << h;
    for (const auto& m : effect.masks) {
        render::ResolvedMask r;
        r.polygon = flattenMaskPath(m.pathAt(t), scale, scale);
        r.feather = std::max(0.0, m.feather.valueAt(t)) * scale;
        r.expansion = m.expansion.valueAt(t) * scale;
        r.opacity = static_cast<float>(std::clamp(m.opacity.valueAt(t), 0.0, 100.0) / 100.0);
        r.subtract = m.mode == model::MaskMode::Subtract;
        r.inverted = m.inverted;
        key << '|' << r.feather << ',' << r.expansion << ',' << r.opacity << ',' << r.subtract << r.inverted;
        for (const auto& p : r.polygon) {
            key << ';' << p.x << ',' << p.y;
        }
        resolved.push_back(std::move(r));
    }
    const auto k = key.str();
    for (auto it = m_masks.begin(); it != m_masks.end(); ++it) {
        if (it->key == k) {
            m_masks.splice(m_masks.begin(), m_masks, it);
            return m_masks.front().image;
        }
    }
    auto image = render::rasterizeMasks(resolved, w, h);
    m_masks.push_front({k, image});
    if (m_masks.size() > kMaskCacheSize) {
        m_masks.pop_back();
    }
    return image;
}

void EffectResolver::apply(const model::Clip& clip, const model::MediaInfo& media, const model::SequenceSettings& settings,
                           const Rational& t, render::Layer& layer) {
    const auto v = [&](std::string_view type, std::string_view key) {
        return static_cast<float>(value(clip, type, key, t, media, settings));
    };
    // Lumetri Color (D33).
    if (const auto* e = enabledEffect(clip, model::kLumetriEffect)) {
        auto& lum = layer.grade.lumetri;
        lum.enabled = true;
        const char* ranges[3] = {"shadows", "midtones", "highlights"};
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string r(ranges[i]);
            lum.wheelOffset[i] = render::wheelOffset(v(model::kLumetriEffect, r + "Hue"), v(model::kLumetriEffect, r + "Amount"));
            lum.wheelLuma[i] = v(model::kLumetriEffect, r + "Luma") / 100.0f * 0.3f;
        }
        std::ostringstream curveKey;
        curveKey.precision(9);
        for (const auto& [name, points] : e->curves) {
            curveKey << name << ':';
            for (const auto& p : points) {
                curveKey << p.x << ',' << p.y << ';';
            }
        }
        auto [it, inserted] = m_curves.try_emplace(curveKey.str());
        if (inserted) {
            it->second = curveTableOf(*e);
        }
        lum.curves = it->second;
        auto& q = lum.hsl;
        q.enabled = v(model::kLumetriEffect, "hslEnabled") >= 0.5f;
        q.hueCenter = v(model::kLumetriEffect, "hslHue");
        q.hueRange = v(model::kLumetriEffect, "hslRange");
        q.softness = v(model::kLumetriEffect, "hslSoftness");
        q.satMin = v(model::kLumetriEffect, "hslSatMin");
        q.lumaMin = v(model::kLumetriEffect, "hslLumaMin");
        q.lumaMax = v(model::kLumetriEffect, "hslLumaMax");
        q.hueShift = v(model::kLumetriEffect, "hslHueShift");
        q.saturation = v(model::kLumetriEffect, "hslSaturation");
        q.luma = v(model::kLumetriEffect, "hslLuma");
        q.showMask = v(model::kLumetriEffect, "hslShowMask") >= 0.5f;
        lum.vignette = {v(model::kLumetriEffect, "vignetteAmount"), v(model::kLumetriEffect, "vignetteMidpoint"),
                        v(model::kLumetriEffect, "vignetteFeather")};
        layer.masks.lumetri = masksFor(*e, t, layer);
    }
    if (const auto* e = enabledEffect(clip, model::kColorEffect)) {
        layer.masks.color = masksFor(*e, t, layer);
    }
    if (const auto* e = enabledEffect(clip, model::kLutEffect)) {
        layer.masks.lut = masksFor(*e, t, layer);
    }
    if (const auto* e = enabledEffect(clip, model::kOpacityEffect)) {
        layer.masks.opacity = masksFor(*e, t, layer);
    }
    // Ultra Key (D35).
    if (const auto* e = enabledEffect(clip, model::kUltraKeyEffect)) {
        auto& k = layer.key;
        k.enabled = true;
        k.keyColor = parseHexColor(e->string("keyColor", "#00ff00"), {0.0f, 1.0f, 0.0f});
        k.tolerance = v(model::kUltraKeyEffect, "tolerance") / 100.0f;
        k.softness = v(model::kUltraKeyEffect, "softness") / 100.0f;
        k.choke = v(model::kUltraKeyEffect, "choke") / 100.0f;
        k.spill = v(model::kUltraKeyEffect, "spill") / 100.0f;
        k.matte = v(model::kUltraKeyEffect, "output") >= 0.5f;
    }
    // Gaussian Blur / Sharpen (D37): sizes are in layer pixels, the prepass works on the decoded frame.
    const double toFrame = layer.frame && layer.sourceWidth > 0 ? layer.frame->width / layer.sourceWidth : 1.0;
    if (const auto* e = enabledEffect(clip, model::kGaussianBlurEffect)) {
        layer.blur.sigma = static_cast<float>(v(model::kGaussianBlurEffect, "blurriness") / 3.0 * toFrame);
        const int dims = static_cast<int>(std::lround(v(model::kGaussianBlurEffect, "dimensions")));
        layer.blur.horizontal = dims != 2;
        layer.blur.vertical = dims != 1;
        layer.masks.blur = masksFor(*e, t, layer);
    }
    if (const auto* e = enabledEffect(clip, model::kSharpenEffect)) {
        layer.blur.sharpen = v(model::kSharpenEffect, "amount") / 100.0f;
        if (!layer.masks.blur) {
            layer.masks.blur = masksFor(*e, t, layer);
        }
    }
    // Warp Stabilizer (D36).
    if (const auto* e = enabledEffect(clip, model::kStabilizerEffect); e != nullptr && !e->samples.empty()) {
        const double smoothness = v(model::kStabilizerEffect, "smoothness") / 100.0;
        const bool full = v(model::kStabilizerEffect, "method") >= 0.5f;
        const bool autoScale = v(model::kStabilizerEffect, "framing") >= 0.5f;
        std::ostringstream key;
        key << e->samples.size() << ':' << e->samples.back() << ':' << smoothness << ':' << full << ':' << autoScale;
        auto& cached = m_plans[e->id];
        if (cached.key != key.str()) {
            cached.key = key.str();
            cached.plan = {};
            if (auto analysis = render::decodeStabilizer(e->samples)) {
                cached.analysis = *analysis;
                cached.plan = render::planStabilization(analysis->motions, smoothness, full, analysis->width,
                                                        analysis->height, autoScale);
            }
        }
        if (!cached.plan.corrections.empty()) {
            layer.pre = cached.plan.corrections[cached.analysis.indexAt(t)];
        }
    }
}

} // namespace loki::playback

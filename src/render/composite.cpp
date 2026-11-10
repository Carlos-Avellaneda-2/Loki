#include "render/composite.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace loki::render {

std::array<double, 9> layerToCanvas(const Layer& layer) noexcept {
    const auto& x = layer.transform;
    const double s = x.scale / 100.0;
    const double r = x.rotation * std::numbers::pi / 180.0;
    const double c = std::cos(r);
    const double n = std::sin(r);
    // M = T(position) * R(rotation) * S(s) * T(-anchor); with y down, positive angles turn clockwise.
    const double a = c * s;
    const double b = -n * s;
    const double d = n * s;
    const double e = c * s;
    const double tx = x.positionX - (a * x.anchorX + b * x.anchorY);
    const double ty = x.positionY - (d * x.anchorX + e * x.anchorY);
    // Stabilizer correction first (in source pixels), then Motion.
    return multiply({a, b, tx, d, e, ty, 0.0, 0.0, 1.0}, layer.pre);
}

std::array<double, 2> transformPoint(const std::array<double, 9>& m, double x, double y) noexcept {
    return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]};
}

std::array<std::array<double, 2>, 4> layerCorners(const Layer& layer) noexcept {
    const auto m = layerToCanvas(layer);
    return {transformPoint(m, 0, 0), transformPoint(m, layer.sourceWidth, 0), transformPoint(m, 0, layer.sourceHeight),
            transformPoint(m, layer.sourceWidth, layer.sourceHeight)};
}

namespace {

float overlay(float s, float d) noexcept {
    return d < 0.5f ? 2.0f * s * d : 1.0f - 2.0f * (1.0f - s) * (1.0f - d);
}

} // namespace

Rgb blendColor(BlendMode mode, const Rgb& src, const Rgb& dst) noexcept {
    Rgb out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float s = src[i];
        const float d = dst[i];
        float v = s;
        switch (mode) {
        case BlendMode::Normal: v = s; break;
        case BlendMode::Multiply: v = s * d; break;
        case BlendMode::Screen: v = 1.0f - (1.0f - s) * (1.0f - d); break;
        case BlendMode::Overlay: v = overlay(s, d); break;
        case BlendMode::Add: v = std::min(1.0f, s + d); break;
        case BlendMode::Darken: v = std::min(s, d); break;
        case BlendMode::Lighten: v = std::max(s, d); break;
        case BlendMode::Difference: v = std::abs(s - d); break;
        }
        out[i] = v;
    }
    return out;
}

Rgb compositePixel(BlendMode mode, const Rgb& src, const Rgb& dst, float opacity) noexcept {
    const auto blended = blendColor(mode, src, dst);
    const float a = std::clamp(opacity, 0.0f, 1.0f);
    return {dst[0] + (blended[0] - dst[0]) * a, dst[1] + (blended[1] - dst[1]) * a, dst[2] + (blended[2] - dst[2]) * a};
}

Rgba compositeOver(BlendMode mode, const Rgb& src, float alpha, const Rgba& dst) noexcept {
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    const float da = std::clamp(dst.alpha, 0.0f, 1.0f);
    const auto b = blendColor(mode, src, dst.rgb);
    const float outA = a + da * (1.0f - a);
    Rgba out;
    out.alpha = outA;
    if (outA <= 0.0f) {
        return out;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        const float blended = src[c] + (b[c] - src[c]) * da;
        out.rgb[c] = (blended * a + dst.rgb[c] * da * (1.0f - a)) / outA;
    }
    return out;
}

void coverCanvas(Layer& layer, int width, int height) noexcept {
    layer.sourceWidth = width;
    layer.sourceHeight = height;
    layer.transform.positionX = width / 2.0;
    layer.transform.positionY = height / 2.0;
    layer.transform.anchorX = width / 2.0;
    layer.transform.anchorY = height / 2.0;
    layer.transform.scale = 100.0;
    layer.transform.rotation = 0.0;
}

} // namespace loki::render

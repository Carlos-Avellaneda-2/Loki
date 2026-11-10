#include "render/grading.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace loki::render {

namespace {

float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

// GLSL smoothstep (edge0 < edge1).
float smoothstep(float e0, float e1, float x) noexcept {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

float luma709(const Rgb& c) noexcept {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

float hueToRgb(float p, float q, float t) noexcept {
    if (t < 0.0f) {
        t += 1.0f;
    }
    if (t > 1.0f) {
        t -= 1.0f;
    }
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

} // namespace

float CurveTable::lookup(std::size_t channel, float v) const noexcept {
    const float x = clamp01(v) * 255.0f;
    const auto i0 = std::min<std::size_t>(static_cast<std::size_t>(x), 255);
    const auto i1 = std::min<std::size_t>(i0 + 1, 255);
    const float f = x - static_cast<float>(i0);
    const auto& t = rgb[channel];
    return t[i0] + (t[i1] - t[i0]) * f;
}

CurveTable CurveTable::identity() noexcept {
    CurveTable table;
    for (auto& channel : table.rgb) {
        for (std::size_t i = 0; i < 256; ++i) {
            channel[i] = static_cast<float>(i) / 255.0f;
        }
    }
    return table;
}

Rgb rgbToHsl(const Rgb& c) noexcept {
    const float mx = std::max({c[0], c[1], c[2]});
    const float mn = std::min({c[0], c[1], c[2]});
    const float l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d < 1e-6f) {
        return {0.0f, 0.0f, l};
    }
    const float s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float h = 0.0f;
    if (mx == c[0]) {
        h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.0f : 0.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return {h * 60.0f, s, l};
}

Rgb hslToRgb(const Rgb& hsl) noexcept {
    const float h = hsl[0] / 360.0f;
    const float s = clamp01(hsl[1]);
    const float l = clamp01(hsl[2]);
    if (s <= 0.0f) {
        return {l, l, l};
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    return {hueToRgb(p, q, h + 1.0f / 3.0f), hueToRgb(p, q, h), hueToRgb(p, q, h - 1.0f / 3.0f)};
}

Rgb wheelOffset(float hueDegrees, float amount) noexcept {
    const auto hue = hslToRgb({hueDegrees, 1.0f, 0.5f});
    const float l = luma709(hue);
    const float k = std::clamp(amount, 0.0f, 100.0f) / 100.0f * 0.25f;
    return {(hue[0] - l) * k, (hue[1] - l) * k, (hue[2] - l) * k};
}

Rgb lumetriPixel(const Lumetri& m, const Rgb& in, float u, float v) noexcept {
    Rgb c = in;
    // Color wheels: shadows / midtones / highlights weighted by luma.
    const float l = clamp01(luma709(c));
    const float ws = (1.0f - l) * (1.0f - l);
    const float wh = l * l;
    const float wm = 1.0f - ws - wh;
    for (std::size_t i = 0; i < 3; ++i) {
        c[i] = clamp01(c[i] + ws * (m.wheelOffset[0][i] + m.wheelLuma[0]) + wm * (m.wheelOffset[1][i] + m.wheelLuma[1]) +
                       wh * (m.wheelOffset[2][i] + m.wheelLuma[2]));
    }
    if (m.curves) {
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] = m.curves->lookup(i, c[i]);
        }
    }
    const auto& q = m.hsl;
    if (q.enabled) {
        const auto hsl = rgbToHsl(c);
        float dh = std::fmod(std::abs(hsl[0] - q.hueCenter), 360.0f);
        dh = std::min(dh, 360.0f - dh);
        const float soft = std::max(q.softness, 0.001f);
        const float qh = 1.0f - smoothstep(q.hueRange, q.hueRange + soft, dh);
        const float qs = smoothstep(q.satMin - soft, q.satMin, hsl[1] * 100.0f);
        const float ql = smoothstep(q.lumaMin - soft, q.lumaMin, hsl[2] * 100.0f) *
                         (1.0f - smoothstep(q.lumaMax, q.lumaMax + soft, hsl[2] * 100.0f));
        const float sel = qh * qs * ql;
        if (q.showMask) {
            c = {sel, sel, sel};
        } else {
            float h = std::fmod(hsl[0] + q.hueShift + 360.0f, 360.0f);
            const auto fixed = hslToRgb({h, clamp01(hsl[1] * q.saturation / 100.0f), clamp01(hsl[2] + q.luma / 200.0f)});
            for (std::size_t i = 0; i < 3; ++i) {
                c[i] += (fixed[i] - c[i]) * sel;
            }
        }
    }
    const auto& vg = m.vignette;
    if (vg.amount != 0.0f) {
        const float du = u - 0.5f;
        const float dv = v - 0.5f;
        const float d = std::sqrt(du * du + dv * dv) / 0.70710678f;
        const float start = vg.midpoint / 100.0f;
        const float f = smoothstep(start, start + std::max(vg.feather / 100.0f, 0.01f), d);
        const float a = std::clamp(vg.amount, -100.0f, 100.0f) / 100.0f;
        for (auto& x : c) {
            x = a < 0.0f ? x * (1.0f + a * f) : x + (1.0f - x) * a * f;
        }
    }
    return c;
}

Rgb adjustColor(const ColorAdjust& a, const Rgb& in) noexcept {
    Rgb c = in;
    const float gain = std::exp2(a.exposure);
    const float contrast = 1.0f + a.contrast / 100.0f;
    for (auto& v : c) {
        v = (v * gain - 0.5f) * contrast + 0.5f;
    }
    const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    const float sat = a.saturation / 100.0f;
    for (auto& v : c) {
        v = luma + (v - luma) * sat;
    }
    const float t = a.temperature / 100.0f;
    c[0] *= 1.0f + 0.1f * t;
    c[2] *= 1.0f - 0.1f * t;
    return {clamp01(c[0]), clamp01(c[1]), clamp01(c[2])};
}

Rgb Lut3D::sample(const Rgb& c) const noexcept {
    if (size < 2 || data.size() < static_cast<std::size_t>(size) * size * size * 3) {
        return c;
    }
    const int n = size - 1;
    float pos[3];
    int i0[3];
    int i1[3];
    float f[3];
    for (int k = 0; k < 3; ++k) {
        const float range = domainMax[k] - domainMin[k];
        const float x = range > 0.0f ? clamp01((c[k] - domainMin[k]) / range) : 0.0f;
        pos[k] = x * static_cast<float>(n);
        i0[k] = std::min(static_cast<int>(pos[k]), n);
        i1[k] = std::min(i0[k] + 1, n);
        f[k] = pos[k] - static_cast<float>(i0[k]);
    }
    const auto at = [&](int r, int g, int b, int ch) {
        return data[(static_cast<std::size_t>(b) * size * size + static_cast<std::size_t>(g) * size +
                     static_cast<std::size_t>(r)) * 3 + static_cast<std::size_t>(ch)];
    };
    Rgb out{};
    for (int ch = 0; ch < 3; ++ch) {
        const float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i0[2], ch) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i0[2], ch) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i1[2], ch) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i1[2], ch) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        out[static_cast<std::size_t>(ch)] = c0 * (1 - f[2]) + c1 * f[2];
    }
    return out;
}

std::shared_ptr<const Lut3D> parseCubeLut(std::string_view text) {
    auto lut = std::make_shared<Lut3D>();
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream words(line);
        std::string first;
        if (!(words >> first)) {
            continue;
        }
        if (first == "TITLE" || first == "LUT_1D_INPUT_RANGE" || first == "LUT_3D_INPUT_RANGE") {
            continue;
        }
        if (first == "LUT_1D_SIZE") {
            throw std::runtime_error("1D LUTs are not supported (only LUT_3D_SIZE)");
        }
        if (first == "LUT_3D_SIZE") {
            if (!(words >> lut->size) || lut->size < 2 || lut->size > 256) {
                throw std::runtime_error("Invalid LUT_3D_SIZE at line " + std::to_string(lineNo));
            }
            lut->data.reserve(static_cast<std::size_t>(lut->size) * lut->size * lut->size * 3);
            continue;
        }
        if (first == "DOMAIN_MIN" || first == "DOMAIN_MAX") {
            auto& d = first == "DOMAIN_MIN" ? lut->domainMin : lut->domainMax;
            if (!(words >> d[0] >> d[1] >> d[2])) {
                throw std::runtime_error("Invalid " + first + " at line " + std::to_string(lineNo));
            }
            continue;
        }
        // Data row: three floats.
        float r = 0;
        float g = 0;
        float b = 0;
        std::istringstream row(line);
        if (!(row >> r >> g >> b)) {
            throw std::runtime_error("Unexpected text at line " + std::to_string(lineNo) + ": " + first);
        }
        lut->data.insert(lut->data.end(), {r, g, b});
    }
    if (lut->size == 0) {
        throw std::runtime_error("Missing LUT_3D_SIZE");
    }
    const auto expected = static_cast<std::size_t>(lut->size) * lut->size * lut->size * 3;
    if (lut->data.size() != expected) {
        throw std::runtime_error("LUT has " + std::to_string(lut->data.size() / 3) + " entries, expected " +
                                 std::to_string(expected / 3));
    }
    return lut;
}

std::shared_ptr<const Lut3D> loadCubeLut(const std::string& utf8Path) {
    std::ifstream file(std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end())), std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open LUT file " + utf8Path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parseCubeLut(buffer.str());
}

float wipeAlpha(const WipeMask& wipe, float canvasX, float canvasY) noexcept {
    if (!wipe.enabled) {
        return 1.0f;
    }
    const float soft = std::max(wipe.softness, 1e-4f);
    // Distance travelled by the edge: 0 at the start; the edge goes from -soft to reach + soft so
    // progress 0 hides everything and 1 shows everything.
    float distance = canvasX;
    float reach = 1.0f;
    if (wipe.shape == WipeShape::Circle) {
        const float dx = (canvasX - 0.5f) * wipe.aspect;
        const float dy = canvasY - 0.5f;
        distance = std::sqrt(dx * dx + dy * dy);
        reach = std::sqrt(0.25f * wipe.aspect * wipe.aspect + 0.25f);
    } else if (wipe.shape == WipeShape::Box) {
        distance = std::max(std::abs(canvasX - 0.5f), std::abs(canvasY - 0.5f)) * 2.0f;
    }
    const float edge = -soft + wipe.progress * (reach + 2.0f * soft);
    const float t = clamp01((edge - distance) / soft + 0.5f);
    const float shown = t * t * (3.0f - 2.0f * t);
    return wipe.reverse ? 1.0f - shown : shown;
}

Rgb gradePixel(const LayerGrade& grade, const Rgb& in, const PixelContext& ctx) noexcept {
    Rgb c = in;
    if (!grade.color.isIdentity()) {
        const auto adjusted = adjustColor(grade.color, c);
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] += (adjusted[i] - c[i]) * ctx.colorMask;
        }
    }
    if (grade.lumetri.enabled) {
        const auto graded = lumetriPixel(grade.lumetri, c, ctx.u, ctx.v);
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] += (graded[i] - c[i]) * ctx.lumetriMask;
        }
    }
    if (grade.lut && grade.lutMix > 0.0f) {
        const auto l = grade.lut->sample(c);
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] += (l[i] - c[i]) * clamp01(grade.lutMix) * ctx.lutMask;
        }
    }
    if (grade.dim > 0.0f) {
        const float d = clamp01(grade.dim);
        for (auto& v : c) {
            v += (grade.dimTarget - v) * d;
        }
    }
    return c;
}

} // namespace loki::render

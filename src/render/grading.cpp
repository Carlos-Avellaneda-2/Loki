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

} // namespace

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

float wipeAlpha(const WipeMask& wipe, float canvasX) noexcept {
    if (!wipe.enabled) {
        return 1.0f;
    }
    const float soft = std::max(wipe.softness, 1e-4f);
    // Edge travels from -soft to 1 + soft so progress 0 hides everything and 1 shows everything.
    const float edge = -soft + wipe.progress * (1.0f + 2.0f * soft);
    const float t = clamp01((edge - canvasX) / soft + 0.5f);
    const float shown = t * t * (3.0f - 2.0f * t);
    return wipe.reverse ? 1.0f - shown : shown;
}

Rgb gradePixel(const LayerGrade& grade, const Rgb& in) noexcept {
    Rgb c = grade.color.isIdentity() ? in : adjustColor(grade.color, in);
    if (grade.lut && grade.lutMix > 0.0f) {
        const auto l = grade.lut->sample(c);
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] += (l[i] - c[i]) * clamp01(grade.lutMix);
        }
    }
    if (grade.dim > 0.0f) {
        for (auto& v : c) {
            v *= 1.0f - clamp01(grade.dim);
        }
    }
    return c;
}

} // namespace loki::render

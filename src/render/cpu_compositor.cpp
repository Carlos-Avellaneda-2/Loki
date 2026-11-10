#include "render/cpu_compositor.h"

#include "render/color_conversion.h"

#include <algorithm>
#include <cmath>

namespace loki::render {

SampledColor sampleFrame(const media::VideoFrame& frame, int x, int y) noexcept {
    x = std::clamp(x, 0, std::max(0, frame.width - 1));
    y = std::clamp(y, 0, std::max(0, frame.height - 1));
    const auto at = [&](int plane, int px, int py) -> std::uint8_t {
        const auto p = static_cast<std::size_t>(plane);
        const auto i = static_cast<std::size_t>(py) * static_cast<std::size_t>(frame.strides[p]) + static_cast<std::size_t>(px);
        return i < frame.planes[p].size() ? frame.planes[p][i] : 0;
    };
    if (frame.format == media::PixelFormat::Rgba) {
        const int px = x * 4;
        return {{at(0, px, y) / 255.0f, at(0, px + 1, y) / 255.0f, at(0, px + 2, y) / 255.0f}, at(0, px + 3, y) / 255.0f};
    }
    const float luma = at(0, x, y) / 255.0f;
    float u = 0.5f;
    float v = 0.5f;
    if (frame.format == media::PixelFormat::Nv12) {
        u = at(1, (x / 2) * 2, y / 2) / 255.0f;
        v = at(1, (x / 2) * 2 + 1, y / 2) / 255.0f;
    } else {
        u = at(1, x / 2, y / 2) / 255.0f;
        v = at(2, x / 2, y / 2) / 255.0f;
    }
    return {convertPixel(yuvToRgb(frame.matrix, frame.range), luma, u, v), 1.0f};
}

namespace {

// Nesting deeper than this is not drawn (the model forbids cycles; this only bounds the work).
constexpr int kMaxNestingDepth = 16;

struct FloatImage {
    int width = 0;
    int height = 0;
    std::vector<Rgba> pixels;

    [[nodiscard]] const Rgba& at(int x, int y) const {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
};

FloatImage compositeFloat(const CompositeFrame& frame, int width, int height, int depth);

void drawLayer(FloatImage& accum, const CompositeFrame& frame, const Layer& layer, int depth) {
    if (!layer.hasPicture() || layer.transform.opacity <= 0.0) {
        return;
    }
    const int width = accum.width;
    const int height = accum.height;
    const double cw = frame.width > 0 ? frame.width : width;
    const double ch = frame.height > 0 ? frame.height : height;
    const float opacity = static_cast<float>(layer.transform.opacity / 100.0);
    if (layer.adjustment) {
        // The picture is the composite so far, graded, over itself.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                auto& dst = accum.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
                const float cx = static_cast<float>((x + 0.5) / width);
                const float alpha = opacity * dst.alpha * wipeAlpha(layer.wipe, cx);
                if (alpha > 0.0f) {
                    dst = compositeOver(layer.transform.blend, gradePixel(layer.grade, dst.rgb), alpha, dst);
                }
            }
        }
        return;
    }
    if (layer.sourceWidth <= 0 || layer.sourceHeight <= 0) {
        return;
    }
    // Inverse of layer -> canvas (affine 2x3).
    const auto m = layerToCanvas(layer);
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::abs(det) < 1e-12) {
        return;
    }
    const double ia = m[4] / det;
    const double ib = -m[1] / det;
    const double id = -m[3] / det;
    const double ie = m[0] / det;
    // Nested sequence: composite it first, at the same scale as this canvas.
    FloatImage nested;
    if (layer.nested) {
        if (depth >= kMaxNestingDepth) {
            return;
        }
        const int nw = std::max(1, static_cast<int>(std::lround(layer.sourceWidth * width / cw)));
        const int nh = std::max(1, static_cast<int>(std::lround(layer.sourceHeight * height / ch)));
        nested = compositeFloat(*layer.nested, nw, nh, depth + 1);
    }
    const double toFrameX = layer.nested ? nested.width / layer.sourceWidth : layer.frame->width / layer.sourceWidth;
    const double toFrameY = layer.nested ? nested.height / layer.sourceHeight : layer.frame->height / layer.sourceHeight;
    for (int y = 0; y < height; ++y) {
        const double cy = (y + 0.5) * ch / height;
        for (int x = 0; x < width; ++x) {
            const double cx = (x + 0.5) * cw / width;
            const double dx = cx - m[2];
            const double dy = cy - m[5];
            const double lx = ia * dx + ib * dy;
            const double ly = id * dx + ie * dy;
            if (lx < 0 || ly < 0 || lx >= layer.sourceWidth || ly >= layer.sourceHeight) {
                continue;
            }
            const int fx = static_cast<int>(lx * toFrameX);
            const int fy = static_cast<int>(ly * toFrameY);
            SampledColor sample;
            if (layer.nested) {
                const auto& p = nested.at(fx, fy);
                sample = {p.rgb, p.alpha};
            } else {
                sample = sampleFrame(*layer.frame, fx, fy);
            }
            const float alpha = opacity * sample.alpha * wipeAlpha(layer.wipe, static_cast<float>(cx / cw));
            if (alpha <= 0.0f) {
                continue;
            }
            auto& dst = accum.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            dst = compositeOver(layer.transform.blend, gradePixel(layer.grade, sample.rgb), alpha, dst);
        }
    }
}

FloatImage compositeFloat(const CompositeFrame& frame, int width, int height, int depth) {
    FloatImage accum;
    accum.width = std::max(0, width);
    accum.height = std::max(0, height);
    const Rgba background{{0.0f, 0.0f, 0.0f}, frame.transparent ? 0.0f : 1.0f};
    accum.pixels.assign(static_cast<std::size_t>(accum.width) * static_cast<std::size_t>(accum.height), background);
    if (accum.width == 0 || accum.height == 0) {
        return accum;
    }
    for (const auto& layer : frame.layers) {
        drawLayer(accum, frame, layer, depth);
    }
    return accum;
}

} // namespace

CpuImage compositeCpu(const CompositeFrame& frame, int width, int height) {
    const auto accum = compositeFloat(frame, width, height, 0);
    CpuImage out;
    out.width = accum.width;
    out.height = accum.height;
    out.rgba.resize(accum.pixels.size() * 4);
    for (std::size_t i = 0; i < accum.pixels.size(); ++i) {
        const auto& p = accum.pixels[i];
        for (std::size_t c = 0; c < 3; ++c) {
            // Over black (a transparent top-level composite shows black where empty).
            const float v = std::clamp(p.rgb[c] * std::clamp(p.alpha, 0.0f, 1.0f), 0.0f, 1.0f);
            out.rgba[i * 4 + c] = static_cast<std::uint8_t>(std::lround(v * 255.0f));
        }
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

} // namespace loki::render

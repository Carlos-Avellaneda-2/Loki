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

CpuImage compositeCpu(const CompositeFrame& frame, int width, int height) {
    CpuImage out;
    out.width = std::max(0, width);
    out.height = std::max(0, height);
    std::vector<Rgb> accum(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height), Rgb{0, 0, 0});
    const double cw = frame.width > 0 ? frame.width : out.width;
    const double ch = frame.height > 0 ? frame.height : out.height;
    for (const auto& layer : frame.layers) {
        if (!layer.frame || layer.frame->planeCount == 0 || layer.transform.opacity <= 0.0 || layer.sourceWidth <= 0 ||
            layer.sourceHeight <= 0) {
            continue;
        }
        // Inverse of layer -> canvas (affine 2x3).
        const auto m = layerToCanvas(layer);
        const double det = m[0] * m[4] - m[1] * m[3];
        if (std::abs(det) < 1e-12) {
            continue;
        }
        const double ia = m[4] / det;
        const double ib = -m[1] / det;
        const double id = -m[3] / det;
        const double ie = m[0] / det;
        const auto& f = *layer.frame;
        const double toFrameX = f.width / layer.sourceWidth;
        const double toFrameY = f.height / layer.sourceHeight;
        const float opacity = static_cast<float>(layer.transform.opacity / 100.0);
        for (int y = 0; y < out.height; ++y) {
            const double cy = (y + 0.5) * ch / out.height;
            for (int x = 0; x < out.width; ++x) {
                const double cx = (x + 0.5) * cw / out.width;
                const double dx = cx - m[2];
                const double dy = cy - m[5];
                const double lx = ia * dx + ib * dy;
                const double ly = id * dx + ie * dy;
                if (lx < 0 || ly < 0 || lx >= layer.sourceWidth || ly >= layer.sourceHeight) {
                    continue;
                }
                const auto sample = sampleFrame(f, static_cast<int>(lx * toFrameX), static_cast<int>(ly * toFrameY));
                const float alpha = opacity * sample.alpha * wipeAlpha(layer.wipe, static_cast<float>(cx / cw));
                if (alpha <= 0.0f) {
                    continue;
                }
                auto& dst = accum[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(x)];
                dst = compositePixel(layer.transform.blend, gradePixel(layer.grade, sample.rgb), dst, alpha);
            }
        }
    }
    out.rgba.resize(accum.size() * 4);
    for (std::size_t i = 0; i < accum.size(); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            out.rgba[i * 4 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(accum[i][c], 0.0f, 1.0f) * 255.0f));
        }
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

} // namespace loki::render

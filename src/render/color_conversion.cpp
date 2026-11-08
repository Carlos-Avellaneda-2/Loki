#include "render/color_conversion.h"

#include <algorithm>

namespace loki::render {

YuvToRgb yuvToRgb(media::ColorMatrix matrix, media::ColorRange range) noexcept {
    // Luma/chroma coefficients (Kr, Kb) of each standard.
    const float kr = matrix == media::ColorMatrix::Bt709 ? 0.2126f : 0.299f;
    const float kb = matrix == media::ColorMatrix::Bt709 ? 0.0722f : 0.114f;
    const float kg = 1.0f - kr - kb;
    // R = Y + a*Cr ; G = Y - b*Cb - c*Cr ; B = Y + d*Cb   (Y in [0,1], Cb/Cr in [-0.5, 0.5])
    const float a = 2.0f * (1.0f - kr);
    const float d = 2.0f * (1.0f - kb);
    const float b = d * kb / kg;
    const float c = a * kr / kg;

    // Range expansion: limited (16-235 / 16-240) or full (0-255).
    float ys = 1.0f;
    float yo = 0.0f;
    float cs = 1.0f;
    if (range == media::ColorRange::Limited) {
        ys = 255.0f / 219.0f;
        yo = -16.0f / 219.0f;
        cs = 255.0f / 224.0f;
    }
    // Chroma centered on code 128: (u*255 - 128)/224 for limited, (u*255 - 128)/255 for full.
    const float cOffset = range == media::ColorRange::Limited ? -128.0f / 224.0f : -128.0f / 255.0f;

    YuvToRgb r;
    // Y' = ys*y + yo ; Cb = cs*u + cOffset ; Cr = cs*v + cOffset
    r.matrix = {ys, 0.0f, a * cs,           //
                ys, -b * cs, -c * cs,        //
                ys, d * cs, 0.0f};
    r.offset = {yo + a * cOffset, yo - b * cOffset - c * cOffset, yo + d * cOffset};
    return r;
}

std::array<float, 3> convertPixel(const YuvToRgb& c, float y, float u, float v) noexcept {
    std::array<float, 3> rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float value = c.matrix[i * 3] * y + c.matrix[i * 3 + 1] * u + c.matrix[i * 3 + 2] * v + c.offset[i];
        rgb[i] = std::clamp(value, 0.0f, 1.0f);
    }
    return rgb;
}

std::array<float, 4> fitRect(float srcWidth, float srcHeight, float dstWidth, float dstHeight) noexcept {
    if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) {
        return {0, 0, 0, 0};
    }
    const float scale = std::min(dstWidth / srcWidth, dstHeight / srcHeight);
    const float w = srcWidth * scale;
    const float h = srcHeight * scale;
    return {(dstWidth - w) / 2.0f, (dstHeight - h) / 2.0f, w, h};
}

} // namespace loki::render

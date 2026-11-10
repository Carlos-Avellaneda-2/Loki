#include "render/scopes.h"

#include <algorithm>
#include <cmath>

namespace loki::render {

YCbCr toYCbCr709(int r, int g, int b) noexcept {
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double cb = (b - y) / 1.8556;
    const double cr = (r - y) / 1.5748;
    const auto clamp8 = [](double v) { return std::clamp(static_cast<int>(std::lround(v)), 0, 255); };
    return {clamp8(y), clamp8(cb + 128.0), clamp8(cr + 128.0)};
}

ScopeData analyzeScopes(const std::uint8_t* rgba, int width, int height, int stride, int maxColumns) {
    ScopeData data;
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return data;
    }
    constexpr int L = ScopeData::kLevels;
    data.columns = std::clamp(maxColumns, 1, width);
    const auto cells = static_cast<std::size_t>(data.columns) * L;
    data.luma.assign(cells, 0);
    for (auto& p : data.parade) {
        p.assign(cells, 0);
    }
    data.vectorscope.assign(static_cast<std::size_t>(L) * L, 0);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = rgba + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const int r = row[x * 4];
            const int g = row[x * 4 + 1];
            const int b = row[x * 4 + 2];
            const auto c = toYCbCr709(r, g, b);
            const auto column = static_cast<std::size_t>(static_cast<std::int64_t>(x) * data.columns / width);
            const auto cols = static_cast<std::size_t>(data.columns);
            ++data.luma[static_cast<std::size_t>(c.y) * cols + column];
            ++data.parade[0][static_cast<std::size_t>(r) * cols + column];
            ++data.parade[1][static_cast<std::size_t>(g) * cols + column];
            ++data.parade[2][static_cast<std::size_t>(b) * cols + column];
            ++data.vectorscope[static_cast<std::size_t>(c.cr) * L + static_cast<std::size_t>(c.cb)];
            ++data.histogram[0][static_cast<std::size_t>(r)];
            ++data.histogram[1][static_cast<std::size_t>(g)];
            ++data.histogram[2][static_cast<std::size_t>(b)];
            ++data.histogram[3][static_cast<std::size_t>(c.y)];
        }
    }
    data.samples = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return data;
}

} // namespace loki::render

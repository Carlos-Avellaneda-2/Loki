#pragma once

#include "media/video_frame.h"

#include <string>

namespace loki::text {

// Everything that defines a title picture (loki.text values resolved for one instant).
struct TitleSpec {
    std::string text;
    std::string font;
    std::string color; // "#rrggbb"
    double size = 96;  // pixels
    double x = 0;      // center of the text block, canvas pixels
    double y = 0;
    bool shadow = true;
    double shadowDistance = 6;
    double shadowOpacity = 60; // percent
    double backgroundOpacity = 0; // percent of a black box behind the text (captions, D40)
    int canvasWidth = 1920;
    int canvasHeight = 1080;

    friend bool operator==(const TitleSpec&, const TitleSpec&) = default;
};

// Rasterizes a title into a canvas-sized RGBA frame (straight alpha) with QtGui (D21). Thread-safe
// (QPainter on a private QImage); needs a QGuiApplication for fonts and returns null without one.
[[nodiscard]] media::VideoFramePtr renderTitle(const TitleSpec& spec);

} // namespace loki::text

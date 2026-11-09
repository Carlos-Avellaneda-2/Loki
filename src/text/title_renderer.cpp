#include "text/title_renderer.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace loki::text {

media::VideoFramePtr renderTitle(const TitleSpec& spec) {
    if (QGuiApplication::instance() == nullptr || spec.canvasWidth <= 0 || spec.canvasHeight <= 0) {
        return nullptr;
    }
    QImage image(spec.canvasWidth, spec.canvasHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        QFont font(QString::fromStdString(spec.font));
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(spec.size))));
        p.setFont(font);
        const QString text = QString::fromStdString(spec.text);
        // Centered block around (x, y); the rectangle is large enough for any canvas.
        const QRectF box(spec.x - spec.canvasWidth * 2.0, spec.y - spec.canvasHeight * 2.0, spec.canvasWidth * 4.0,
                         spec.canvasHeight * 4.0);
        if (spec.shadow && spec.shadowOpacity > 0.0) {
            const double d = spec.shadowDistance / std::sqrt(2.0);
            QColor shadow(Qt::black);
            shadow.setAlphaF(static_cast<float>(std::clamp(spec.shadowOpacity / 100.0, 0.0, 1.0)));
            p.setPen(shadow);
            p.drawText(box.translated(d, d), Qt::AlignCenter, text);
        }
        QColor color(QString::fromStdString(spec.color));
        p.setPen(color.isValid() ? color : QColor(Qt::white));
        p.drawText(box, Qt::AlignCenter, text);
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888); // straight alpha
    auto frame = std::make_shared<media::VideoFrame>();
    frame->format = media::PixelFormat::Rgba;
    frame->width = rgba.width();
    frame->height = rgba.height();
    frame->range = media::ColorRange::Full;
    frame->planeCount = 1;
    frame->strides[0] = rgba.width() * 4;
    frame->planeHeights[0] = rgba.height();
    frame->planes[0].resize(static_cast<std::size_t>(frame->strides[0]) * static_cast<std::size_t>(rgba.height()));
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(frame->planes[0].data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame->strides[0]),
                    rgba.constScanLine(y), static_cast<std::size_t>(frame->strides[0]));
    }
    return frame;
}

} // namespace loki::text

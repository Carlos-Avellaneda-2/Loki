#include "ui/scrub_bar.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace loki::ui {

namespace {
constexpr int kMargin = 8;
}

ScrubBar::ScrubBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
}

void ScrubBar::setTiming(const Rational& duration, const Rational& frameRate) {
    m_duration = duration;
    m_frameRate = frameRate.isPositive() ? frameRate : Rational(30);
    update();
}

void ScrubBar::setPosition(const Rational& position) {
    if (position != m_position) {
        m_position = position;
        update();
    }
}

void ScrubBar::setMarks(std::optional<Rational> in, std::optional<Rational> out) {
    m_in = in;
    m_out = out;
    update();
}

qreal ScrubBar::xOf(const Rational& t) const {
    if (!m_duration.isPositive()) {
        return kMargin;
    }
    const qreal span = width() - 2 * kMargin;
    return kMargin + span * std::clamp((t / m_duration).toDouble(), 0.0, 1.0);
}

Rational ScrubBar::timeAt(int x) const {
    if (!m_duration.isPositive()) {
        return {};
    }
    const int span = std::max(1, width() - 2 * kMargin);
    const int px = std::clamp(x - kMargin, 0, span);
    const Rational t = m_duration * Rational(px, span);
    const Rational last = max(Rational(), m_duration - m_frameRate.reciprocal());
    return min(quantizeToFrame(t, m_frameRate, Rounding::Floor), last);
}

void ScrubBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int h = height();
    p.fillRect(rect(), QColor(0x1b, 0x1b, 0x1b));
    // In/Out range.
    if (m_in || m_out) {
        const qreal x0 = xOf(m_in.value_or(Rational()));
        const qreal x1 = xOf(m_out.value_or(m_duration));
        p.fillRect(QRectF(x0, 0, std::max<qreal>(1, x1 - x0), h), QColor(0x2d, 0x8c, 0xeb, 70));
        p.setPen(QColor(0x2d, 0x8c, 0xeb));
        if (m_in) {
            p.drawLine(QPointF(x0, 0), QPointF(x0, h));
        }
        if (m_out) {
            p.drawLine(QPointF(x1, 0), QPointF(x1, h));
        }
    }
    // Ticks: ~10 per bar.
    p.setPen(QColor(0x50, 0x50, 0x50));
    for (int i = 0; i <= 10; ++i) {
        const qreal x = kMargin + (width() - 2 * kMargin) * i / 10.0;
        p.drawLine(QPointF(x, h - 6), QPointF(x, h));
    }
    // Playhead.
    const qreal x = xOf(m_position);
    p.setPen(QPen(QColor(0x2d, 0x8c, 0xeb), 2));
    p.drawLine(QPointF(x, 0), QPointF(x, h));
    p.setBrush(QColor(0x2d, 0x8c, 0xeb));
    p.setPen(Qt::NoPen);
    const QPointF head[] = {QPointF(x - 5, 0), QPointF(x + 5, 0), QPointF(x, 7)};
    p.drawPolygon(head, 3);
}

void ScrubBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_dragging = true;
    emit scrubStarted();
    m_position = timeAt(static_cast<int>(event->position().x()));
    update();
    emit scrubbed(m_position);
}

void ScrubBar::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        return;
    }
    const auto t = timeAt(static_cast<int>(event->position().x()));
    if (t != m_position) {
        m_position = t;
        update();
        emit scrubbed(t);
    }
}

void ScrubBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        emit scrubFinished();
    }
}

} // namespace loki::ui

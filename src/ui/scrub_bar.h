#pragma once

#include "util/rational.h"

#include <QWidget>

#include <optional>

namespace loki::ui {

// Monitor time bar: shows duration ticks, the In/Out range and the playhead; click or drag to
// scrub (positions are snapped to whole frames).
class ScrubBar final : public QWidget {
    Q_OBJECT

public:
    explicit ScrubBar(QWidget* parent = nullptr);

    void setTiming(const Rational& duration, const Rational& frameRate);
    void setPosition(const Rational& position);
    void setMarks(std::optional<Rational> in, std::optional<Rational> out);

    [[nodiscard]] Rational position() const noexcept { return m_position; }
    [[nodiscard]] QSize sizeHint() const override { return {400, 28}; }
    // Frame-snapped time at widget x coordinate.
    [[nodiscard]] Rational timeAt(int x) const;

signals:
    void scrubStarted();
    void scrubbed(const loki::Rational& time);
    void scrubFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] qreal xOf(const Rational& t) const;

    Rational m_duration;
    Rational m_frameRate{30};
    Rational m_position;
    std::optional<Rational> m_in;
    std::optional<Rational> m_out;
    bool m_dragging = false;
};

} // namespace loki::ui

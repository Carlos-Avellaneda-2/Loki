#include "ui/curve_editor.h"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace loki::ui {

namespace {

const char* const kChannels[] = {"master", "red", "green", "blue"};

QColor channelColor(const std::string& name) {
    if (name == "red") {
        return {0xff, 0x60, 0x60};
    }
    if (name == "green") {
        return {0x60, 0xe0, 0x60};
    }
    if (name == "blue") {
        return {0x70, 0x90, 0xff};
    }
    return {0xe0, 0xe0, 0xe0};
}

} // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    m_channel = new QComboBox(this);
    m_channel->addItems({tr("Master (Luma)"), tr("Red"), tr("Green"), tr("Blue")});
    m_canvas = new CurveCanvas(*this, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_channel);
    layout->addWidget(m_canvas);
    connect(m_channel, &QComboBox::currentIndexChanged, m_canvas, qOverload<>(&QWidget::update));
}

void CurveEditor::setCurves(const std::map<std::string, std::vector<model::CurvePoint>>& curves) {
    if (m_canvas->dragging()) {
        return;
    }
    m_curves = curves;
    m_canvas->update();
}

std::string CurveEditor::channel() const {
    return kChannels[std::clamp(m_channel->currentIndex(), 0, 3)];
}

void CurveEditor::setChannel(const std::string& name) {
    for (int i = 0; i < 4; ++i) {
        if (name == kChannels[i]) {
            m_channel->setCurrentIndex(i);
        }
    }
}

std::vector<model::CurvePoint> CurveEditor::points() const {
    const auto it = m_curves.find(channel());
    return it == m_curves.end() ? model::identityCurve() : it->second;
}

void CurveEditor::emitEdit(bool finished) {
    emit curveEdited(QString::fromStdString(channel()), points(), finished);
}

CurveCanvas::CurveCanvas(CurveEditor& editor, QWidget* parent) : QWidget(parent), m_editor(editor) {
    setMinimumSize(160, 140);
}

std::vector<model::CurvePoint>& CurveCanvas::curve() {
    auto& c = m_editor.m_curves[m_editor.channel()];
    if (c.size() < 2) {
        c = model::identityCurve();
    }
    return c;
}

QRectF CurveCanvas::box() const {
    const double side = std::min(width(), height()) - 8.0;
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QPointF CurveCanvas::toWidget(const model::CurvePoint& p) const {
    const auto b = box();
    return {b.left() + p.x * b.width(), b.bottom() - p.y * b.height()};
}

model::CurvePoint CurveCanvas::toCurve(const QPointF& w) const {
    const auto b = box();
    return {std::clamp((w.x() - b.left()) / b.width(), 0.0, 1.0), std::clamp((b.bottom() - w.y()) / b.height(), 0.0, 1.0)};
}

std::optional<std::size_t> CurveCanvas::pointAt(const QPointF& w) const {
    const auto it = m_editor.m_curves.find(m_editor.channel());
    const auto points = it == m_editor.m_curves.end() ? model::identityCurve() : it->second;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto p = toWidget(points[i]);
        if (std::hypot(p.x() - w.x(), p.y() - w.y()) <= 6.0) {
            return i;
        }
    }
    return std::nullopt;
}

void CurveCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));
    const auto b = box();
    p.setPen(QPen(QColor(0x44, 0x44, 0x44), 1));
    for (int i = 0; i <= 4; ++i) {
        const double x = b.left() + b.width() * i / 4.0;
        const double y = b.top() + b.height() * i / 4.0;
        p.drawLine(QPointF(x, b.top()), QPointF(x, b.bottom()));
        p.drawLine(QPointF(b.left(), y), QPointF(b.right(), y));
    }
    const auto it = m_editor.m_curves.find(m_editor.channel());
    const auto points = it == m_editor.m_curves.end() ? model::identityCurve() : it->second;
    QPainterPath path;
    for (int i = 0; i <= 128; ++i) {
        const double x = i / 128.0;
        const auto w = toWidget({x, model::evaluateCurve(points, x)});
        i == 0 ? path.moveTo(w) : path.lineTo(w);
    }
    const auto color = channelColor(m_editor.channel());
    p.setPen(QPen(color, 1.6));
    p.drawPath(path);
    p.setBrush(color);
    for (const auto& pt : points) {
        const auto w = toWidget(pt);
        p.drawEllipse(w, 4.0, 4.0);
    }
}

void CurveCanvas::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    auto& points = curve();
    const auto hit = pointAt(pos);
    if (event->button() == Qt::RightButton) {
        if (hit && *hit > 0 && *hit + 1 < points.size()) {
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(*hit));
            update();
            m_editor.emitEdit(true);
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (hit) {
        m_drag = hit;
        return;
    }
    const auto c = toCurve(pos);
    const auto at = std::upper_bound(points.begin(), points.end(), c.x,
                                     [](double x, const model::CurvePoint& p) { return x < p.x; });
    if (at == points.begin() || at == points.end() || std::prev(at)->x == c.x) {
        return;
    }
    m_drag = static_cast<std::size_t>(std::distance(points.begin(), points.insert(at, c)));
    update();
    m_editor.emitEdit(false);
}

void CurveCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!m_drag) {
        return;
    }
    auto& points = curve();
    const auto i = *m_drag;
    auto c = toCurve(event->position());
    if (i == 0 || i + 1 == points.size()) {
        c.x = points[i].x; // end points move vertically only
    } else {
        constexpr double kGap = 0.01;
        c.x = std::clamp(c.x, points[i - 1].x + kGap, points[i + 1].x - kGap);
    }
    points[i] = c;
    update();
    m_editor.emitEdit(false);
}

void CurveCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (m_drag) {
        m_drag.reset();
        m_editor.emitEdit(true);
    }
}

} // namespace loki::ui

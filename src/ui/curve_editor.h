#pragma once

#include "model/mask.h"

#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QComboBox;

namespace loki::ui {

class CurveCanvas;

// Lumetri RGB curves (D33): master, red, green, blue. Click to add a point, drag to move it, right
// click (or drag out of the box) to remove it; the end points only move up and down.
class CurveEditor final : public QWidget {
    Q_OBJECT

public:
    explicit CurveEditor(QWidget* parent = nullptr);

    // Curves by name ("master", "red", ...); a missing curve is the identity. Ignored mid-drag.
    void setCurves(const std::map<std::string, std::vector<model::CurvePoint>>& curves);
    [[nodiscard]] std::string channel() const;
    void setChannel(const std::string& name);
    [[nodiscard]] std::vector<model::CurvePoint> points() const;

signals:
    // `finished` = the drag ended (merge the edits of one drag into one undo step).
    void curveEdited(const QString& channel, const std::vector<loki::model::CurvePoint>& points, bool finished);

private:
    friend class CurveCanvas;
    void emitEdit(bool finished);

    QComboBox* m_channel = nullptr;
    CurveCanvas* m_canvas = nullptr;
    std::map<std::string, std::vector<model::CurvePoint>> m_curves;
};

class CurveCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit CurveCanvas(CurveEditor& editor, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {220, 180}; }
    [[nodiscard]] bool dragging() const noexcept { return m_drag.has_value(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF box() const;
    [[nodiscard]] QPointF toWidget(const model::CurvePoint& p) const;
    [[nodiscard]] model::CurvePoint toCurve(const QPointF& w) const;
    [[nodiscard]] std::optional<std::size_t> pointAt(const QPointF& w) const;
    std::vector<model::CurvePoint>& curve();

    CurveEditor& m_editor;
    std::optional<std::size_t> m_drag;
};

} // namespace loki::ui

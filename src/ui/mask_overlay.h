#pragma once

#include "commands/clip_commands.h"
#include "model/types.h"
#include "render/effects.h"

#include <QObject>
#include <QPointF>
#include <QRectF>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QPainter;

namespace loki::ui {

class ProjectController;
class VideoView;

// Edits the selected effect mask on the Program Monitor (D34): draws its path over the picture,
// drags vertices or the whole mask, and in pen mode creates a new mask by clicking points (drag to
// make a smooth point; click the first point to close). Animated (tracked) paths are edited at the
// playhead: the key there is changed, or added.
class MaskOverlay final : public QObject {
    Q_OBJECT

public:
    MaskOverlay(ProjectController& controller, VideoView& view, std::function<Rational()> playhead,
                QObject* parent = nullptr);

    // Mask to show and edit (null mask = none).
    void select(model::ClipId clip, std::string effectType, model::MaskId mask);
    // Pen mode: the next clicks draw a new mask on the effect.
    void startPen(model::ClipId clip, std::string effectType);
    void cancel();
    [[nodiscard]] bool penActive() const noexcept { return m_pen; }
    [[nodiscard]] model::MaskId selectedMask() const noexcept { return m_mask; }

    // Mouse input in canvas pixels (public for tests). `tolerance` = canvas pixels per widget pixel.
    bool mouse(int type, Qt::MouseButton button, const QPointF& canvas, double tolerance);
    // Canvas <-> media (layer) pixels for the selected clip at the playhead.
    [[nodiscard]] std::optional<render::Matrix3> layerToCanvasNow() const;

signals:
    void maskCreated(loki::model::MaskId id);

private:
    void paint(QPainter& painter, const QRectF& picture, const QSizeF& canvas);
    [[nodiscard]] const model::Clip* clip() const;
    [[nodiscard]] const model::Mask* mask() const;
    [[nodiscard]] Rational sourceTime() const;
    [[nodiscard]] std::optional<QPointF> toLayer(const QPointF& canvas) const;
    void commitPath(std::vector<model::MaskVertex> path);

    ProjectController& m_controller;
    VideoView& m_view;
    std::function<Rational()> m_playhead;
    model::ClipId m_clip;
    std::string m_effect;
    model::MaskId m_mask;
    bool m_pen = false;
    std::vector<model::MaskVertex> m_penPath; // layer pixels
    bool m_penDragging = false;
    // Drag state: vertex index or -1 for the whole mask.
    std::optional<int> m_dragVertex;
    QPointF m_dragStart;
    std::vector<model::MaskVertex> m_dragOriginal;
    commands::GestureId m_gesture = 0;
};

} // namespace loki::ui

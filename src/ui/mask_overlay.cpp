#include "ui/mask_overlay.h"

#include "commands/effect_commands.h"
#include "model/effect_registry.h"
#include "playback/effect_resolve.h"
#include "render/composite.h"
#include "ui/project_controller.h"
#include "ui/video_view.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace loki::ui {

namespace {

constexpr double kHandlePixels = 7.0; // grab distance in widget pixels

QPointF apply(const render::Matrix3& m, double x, double y) {
    return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]};
}

bool invert(const render::Matrix3& m, render::Matrix3& out) {
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::abs(det) < 1e-12) {
        return false;
    }
    const double ia = m[4] / det;
    const double ib = -m[1] / det;
    const double id = -m[3] / det;
    const double ie = m[0] / det;
    out = {ia, ib, -(ia * m[2] + ib * m[5]), id, ie, -(id * m[2] + ie * m[5]), 0.0, 0.0, 1.0};
    return true;
}

bool inside(const std::vector<render::MaskPoint>& poly, double x, double y) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto& a = poly[j];
        const auto& b = poly[i];
        if ((b.y > y) != (a.y > y) && x < (a.x - b.x) * (y - b.y) / (a.y - b.y) + b.x) {
            in = !in;
        }
    }
    return in;
}

} // namespace

MaskOverlay::MaskOverlay(ProjectController& controller, VideoView& view, std::function<Rational()> playhead, QObject* parent)
    : QObject(parent), m_controller(controller), m_view(view), m_playhead(std::move(playhead)) {
    VideoOverlay overlay;
    overlay.paint = [this](QPainter& p, const QRectF& picture, const QSizeF& canvas) { paint(p, picture, canvas); };
    overlay.mouse = [this](int type, Qt::MouseButton button, Qt::KeyboardModifiers, const QPointF& canvas, double scale) {
        return mouse(type, button, canvas, scale);
    };
    m_view.setOverlay(std::move(overlay));
    connect(&m_controller, &ProjectController::projectChanged, this, [this] { m_view.updateOverlay(); });
}

void MaskOverlay::select(model::ClipId clip, std::string effectType, model::MaskId mask) {
    m_clip = clip;
    m_effect = std::move(effectType);
    m_mask = mask;
    m_pen = false;
    m_penPath.clear();
    m_view.updateOverlay();
}

void MaskOverlay::startPen(model::ClipId clip, std::string effectType) {
    m_clip = clip;
    m_effect = std::move(effectType);
    m_mask = {};
    m_pen = true;
    m_penPath.clear();
    m_view.updateOverlay();
}

void MaskOverlay::cancel() {
    m_pen = false;
    m_penPath.clear();
    m_dragVertex.reset();
    m_view.updateOverlay();
}

const model::Clip* MaskOverlay::clip() const {
    return m_clip ? m_controller.project().findClip(m_clip).clip : nullptr;
}

const model::Mask* MaskOverlay::mask() const {
    const auto* c = clip();
    const auto* effect = c != nullptr ? model::findEffectByType(*c, m_effect) : nullptr;
    return effect != nullptr ? effect->findMask(m_mask) : nullptr;
}

Rational MaskOverlay::sourceTime() const {
    const auto* c = clip();
    if (c == nullptr || !m_playhead) {
        return {};
    }
    return c->toSourceTime(max(c->start, min(m_playhead(), c->end())));
}

std::optional<render::Matrix3> MaskOverlay::layerToCanvasNow() const {
    const auto* c = clip();
    if (c == nullptr) {
        return std::nullopt;
    }
    const auto ctx = commands::clipContext(m_controller.project(), m_clip);
    render::Layer layer;
    const auto* media = m_controller.project().findMedia(c->media);
    if (media != nullptr && media->isAdjustment()) {
        render::coverCanvas(layer, ctx.settings.width, ctx.settings.height);
        return render::layerToCanvas(layer);
    }
    layer.sourceWidth = ctx.media.width > 0 ? ctx.media.width : ctx.settings.width;
    layer.sourceHeight = ctx.media.height > 0 ? ctx.media.height : ctx.settings.height;
    layer.transform = model::resolveTransform(*c, sourceTime(), ctx.media, ctx.settings);
    return render::layerToCanvas(layer);
}

std::optional<QPointF> MaskOverlay::toLayer(const QPointF& canvas) const {
    const auto m = layerToCanvasNow();
    render::Matrix3 inv;
    if (!m || !invert(*m, inv)) {
        return std::nullopt;
    }
    return apply(inv, canvas.x(), canvas.y());
}

void MaskOverlay::paint(QPainter& p, const QRectF& picture, const QSizeF& canvas) {
    const auto m = layerToCanvasNow();
    if (!m || canvas.width() <= 0) {
        return;
    }
    const double s = picture.width() / canvas.width();
    const auto toWidget = [&](double x, double y) {
        const auto c = apply(*m, x, y);
        return QPointF(picture.left() + c.x() * s, picture.top() + c.y() * s);
    };
    const auto drawPath = [&](const std::vector<model::MaskVertex>& path, bool closed, const QColor& color) {
        if (path.empty()) {
            return;
        }
        // Bezier segments: the transform is affine, so mapping the control points maps the curve.
        QPainterPath shape;
        shape.moveTo(toWidget(path.front().x, path.front().y));
        const std::size_t edges = closed ? path.size() : path.size() - 1;
        for (std::size_t i = 0; i < edges; ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size()];
            shape.cubicTo(toWidget(a.x + a.outX, a.y + a.outY), toWidget(b.x + b.inX, b.y + b.inY), toWidget(b.x, b.y));
        }
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
        p.setBrush(color);
        for (const auto& v : path) {
            const auto w = toWidget(v.x, v.y);
            p.drawRect(QRectF(w.x() - 3, w.y() - 3, 6, 6));
        }
    };
    if (m_pen) {
        drawPath(m_penPath, false, QColor(0xff, 0xd0, 0x40));
        return;
    }
    if (const auto* mk = mask()) {
        drawPath(mk->pathAt(sourceTime()), true, QColor(0x40, 0xb0, 0xff));
    }
}

void MaskOverlay::commitPath(std::vector<model::MaskVertex> path) {
    const auto* mk = mask();
    if (mk == nullptr) {
        return;
    }
    auto edited = *mk;
    if (edited.keyframes.empty()) {
        edited.path = std::move(path);
    } else {
        const Rational t = sourceTime();
        auto it = std::find_if(edited.keyframes.begin(), edited.keyframes.end(), [&](const auto& k) { return k.time == t; });
        if (it != edited.keyframes.end()) {
            it->path = std::move(path);
        } else {
            edited.keyframes.push_back({t, std::move(path)});
            std::sort(edited.keyframes.begin(), edited.keyframes.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        }
    }
    try {
        if (auto command = commands::updateEffectMask(m_controller.project(), m_clip, m_effect, edited, m_gesture)) {
            m_controller.execute(std::move(command));
        }
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

bool MaskOverlay::mouse(int type, Qt::MouseButton button, const QPointF& canvas, double scale) {
    const auto local = toLayer(canvas);
    const auto m = layerToCanvasNow();
    if (!local || !m || clip() == nullptr) {
        return false;
    }
    const double grab = kHandlePixels * scale; // canvas pixels
    const auto canvasOf = [&](double x, double y) { return apply(*m, x, y); };
    const auto near = [&](const model::MaskVertex& v) {
        const auto c = canvasOf(v.x, v.y);
        return std::hypot(c.x() - canvas.x(), c.y() - canvas.y()) <= grab;
    };
    if (m_pen) {
        if (type == QEvent::MouseButtonPress && button == Qt::LeftButton) {
            if (m_penPath.size() >= 3 && near(m_penPath.front())) {
                model::Mask created;
                created.path = m_penPath;
                model::MaskId id;
                try {
                    if (auto command = commands::addEffectMask(m_controller.project(), m_clip, m_effect, created, &id)) {
                        m_controller.execute(std::move(command));
                    }
                } catch (const std::exception& e) {
                    emit m_controller.commandFailed(QString::fromUtf8(e.what()));
                }
                m_pen = false;
                m_penPath.clear();
                m_mask = id;
                emit maskCreated(id);
            } else {
                m_penPath.push_back({local->x(), local->y()});
                m_penDragging = true;
            }
            m_view.updateOverlay();
            return true;
        }
        if (type == QEvent::MouseMove && m_penDragging && !m_penPath.empty()) {
            auto& v = m_penPath.back();
            v.outX = local->x() - v.x;
            v.outY = local->y() - v.y;
            v.inX = -v.outX;
            v.inY = -v.outY;
            m_view.updateOverlay();
            return true;
        }
        if (type == QEvent::MouseButtonRelease) {
            m_penDragging = false;
            if (!m_penPath.empty() && std::hypot(m_penPath.back().outX, m_penPath.back().outY) < grab / 2) {
                auto& v = m_penPath.back();
                v.inX = v.inY = v.outX = v.outY = 0.0; // a click (no drag): sharp corner
            }
            return true;
        }
        return false;
    }
    const auto* mk = mask();
    if (mk == nullptr) {
        return false;
    }
    if (type == QEvent::MouseButtonPress && button == Qt::LeftButton) {
        const auto path = mk->pathAt(sourceTime());
        m_dragVertex.reset();
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (near(path[i])) {
                m_dragVertex = static_cast<int>(i);
                break;
            }
        }
        if (!m_dragVertex && inside(playback::flattenMaskPath(path, 1.0, 1.0), local->x(), local->y())) {
            m_dragVertex = -1; // the whole mask
        }
        if (!m_dragVertex) {
            return false;
        }
        m_dragStart = *local;
        m_dragOriginal = path;
        m_gesture = m_controller.newGestureId();
        return true;
    }
    if (type == QEvent::MouseMove && m_dragVertex) {
        const double dx = local->x() - m_dragStart.x();
        const double dy = local->y() - m_dragStart.y();
        auto path = m_dragOriginal;
        if (*m_dragVertex < 0) {
            path = model::transformedPath(path, dx, dy);
        } else {
            auto& v = path[static_cast<std::size_t>(*m_dragVertex)];
            v.x += dx;
            v.y += dy;
        }
        commitPath(std::move(path));
        return true;
    }
    if (type == QEvent::MouseButtonRelease && m_dragVertex) {
        m_dragVertex.reset();
        m_gesture = 0;
        return true;
    }
    return false;
}

} // namespace loki::ui

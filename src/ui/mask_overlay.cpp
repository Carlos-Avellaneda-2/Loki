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
    overlay.mouse = [this](int type, Qt::MouseButton button, Qt::KeyboardModifiers modifiers, const QPointF& canvas,
                           double scale) { return mouse(type, button, canvas, scale, modifiers); };
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

void MaskOverlay::setMotionClip(model::ClipId clip) {
    m_motionClip = clip;
    m_motionDragging = false;
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

// --- Motion ----------------------------------------------------------------------------------------

Rational MaskOverlay::motionSourceTime() const {
    const auto* c = m_motionClip ? m_controller.project().findClip(m_motionClip).clip : nullptr;
    if (c == nullptr || !m_playhead) {
        return {};
    }
    return c->toSourceTime(max(c->start, min(m_playhead(), c->end())));
}

MaskOverlay::MotionPath MaskOverlay::motionPath() const {
    MotionPath path;
    const auto* c = m_motionClip ? m_controller.project().findClip(m_motionClip).clip : nullptr;
    if (c == nullptr) {
        return path;
    }
    const auto ctx = commands::clipContext(m_controller.project(), m_motionClip);
    const auto* motion = model::findEffectByType(*c, model::kMotionEffect);
    const auto* px = motion != nullptr ? motion->findParameter("positionX") : nullptr;
    const auto* py = motion != nullptr ? motion->findParameter("positionY") : nullptr;
    if ((px == nullptr || !px->isAnimated()) && (py == nullptr || !py->isAnimated())) {
        return path; // a still position has no path
    }
    const auto positionAt = [&](const Rational& t) {
        return QPointF(model::effectiveValue(*c, model::kMotionEffect, "positionX", t, ctx.media, ctx.settings),
                       model::effectiveValue(*c, model::kMotionEffect, "positionY", t, ctx.media, ctx.settings));
    };
    const Rational lo = min(c->sourceIn, c->sourceOut());
    const Rational hi = max(c->sourceIn, c->sourceOut());
    constexpr int kSamples = 120;
    for (int i = 0; i <= kSamples; ++i) {
        path.samples.push_back(positionAt(lo + (hi - lo) * Rational(i, kSamples)));
    }
    std::vector<Rational> times;
    for (const auto* p : {px, py}) {
        if (p != nullptr) {
            for (const auto& k : p->keyframes) {
                if (std::find(times.begin(), times.end(), k.time) == times.end()) {
                    times.push_back(k.time);
                }
            }
        }
    }
    std::sort(times.begin(), times.end());
    for (const auto& t : times) {
        path.keys.emplace_back(t, positionAt(t));
    }
    return path;
}

void MaskOverlay::paintMotion(QPainter& p, const QRectF& picture, const QSizeF& canvas) {
    const auto* c = m_controller.project().findClip(m_motionClip).clip;
    if (c == nullptr || canvas.width() <= 0) {
        return;
    }
    const double s = picture.width() / canvas.width();
    const auto toWidget = [&](const QPointF& q) { return QPointF(picture.left() + q.x() * s, picture.top() + q.y() * s); };
    // Motion path with its keyframes.
    const auto path = motionPath();
    if (path.samples.size() > 1) {
        QPolygonF line;
        for (const auto& q : path.samples) {
            line << toWidget(q);
        }
        p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1.2, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(0xff, 0xd0, 0x40));
        for (const auto& [t, q] : path.keys) {
            p.drawEllipse(toWidget(q), 4.5, 4.5);
        }
    }
    // Bounding box and center while the playhead is over the clip.
    const Rational now = m_playhead ? m_playhead() : Rational();
    const auto m = [&]() -> std::optional<render::Matrix3> {
        const auto saved = m_clip;
        auto* self = const_cast<MaskOverlay*>(this);
        self->m_clip = m_motionClip;
        auto r = layerToCanvasNow();
        self->m_clip = saved;
        return r;
    }();
    if (!m || now < c->start || !(now < c->end())) {
        return;
    }
    const auto ctx = commands::clipContext(m_controller.project(), m_motionClip);
    const double w = ctx.media.width > 0 ? ctx.media.width : ctx.settings.width;
    const double h = ctx.media.height > 0 ? ctx.media.height : ctx.settings.height;
    QPolygonF box;
    for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{w, 0.0}, std::pair{w, h}, std::pair{0.0, h}}) {
        box << toWidget(apply(*m, x, y));
    }
    p.setPen(QPen(QColor(0x40, 0xb0, 0xff), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(box);
    const auto center = toWidget(QPointF(
        model::effectiveValue(*c, model::kMotionEffect, "positionX", motionSourceTime(), ctx.media, ctx.settings),
        model::effectiveValue(*c, model::kMotionEffect, "positionY", motionSourceTime(), ctx.media, ctx.settings)));
    p.drawLine(center - QPointF(6, 0), center + QPointF(6, 0));
    p.drawLine(center - QPointF(0, 6), center + QPointF(0, 6));
}

bool MaskOverlay::motionMouse(int type, Qt::MouseButton button, const QPointF& canvas, double scale, bool alt) {
    const auto* c = m_motionClip ? m_controller.project().findClip(m_motionClip).clip : nullptr;
    if (c == nullptr) {
        return false;
    }
    const auto commitPosition = [&](const QPointF& position, const Rational& sourceTime) {
        try {
            if (auto command = commands::setEffectParameters(m_controller.project(), m_motionClip, model::kMotionEffect,
                                                             {{"positionX", position.x()}, {"positionY", position.y()}},
                                                             sourceTime, m_gesture)) {
                m_controller.execute(std::move(command));
            }
        } catch (const std::exception& e) {
            emit m_controller.commandFailed(QString::fromUtf8(e.what()));
        }
    };
    if (type == QEvent::MouseButtonPress && button == Qt::LeftButton) {
        const double grab = kHandlePixels * scale;
        // Is the click on the picture (playhead over the clip)?
        const Rational now = m_playhead ? m_playhead() : Rational();
        const auto ctx = commands::clipContext(m_controller.project(), m_motionClip);
        const double w = ctx.media.width > 0 ? ctx.media.width : ctx.settings.width;
        const double h = ctx.media.height > 0 ? ctx.media.height : ctx.settings.height;
        bool onPicture = false;
        if (c->start <= now && now < c->end()) {
            const auto saved = m_clip;
            m_clip = m_motionClip;
            const auto local = toLayer(canvas);
            m_clip = saved;
            onPicture = local && local->x() >= 0 && local->y() >= 0 && local->x() <= w && local->y() <= h;
        }
        // A keyframe dot of the path: outside the picture, or with Alt.
        if (alt || !onPicture) {
            for (const auto& [t, q] : motionPath().keys) {
                if (std::hypot(q.x() - canvas.x(), q.y() - canvas.y()) <= grab) {
                    m_motionKey = t;
                    m_motionOriginal = q;
                    m_motionStart = canvas;
                    m_motionDragging = true;
                    m_gesture = m_controller.newGestureId();
                    return true;
                }
            }
        }
        if (!onPicture) {
            return false;
        }
        const Rational t = motionSourceTime();
        m_motionKey.reset();
        m_motionOriginal = QPointF(model::effectiveValue(*c, model::kMotionEffect, "positionX", t, ctx.media, ctx.settings),
                                   model::effectiveValue(*c, model::kMotionEffect, "positionY", t, ctx.media, ctx.settings));
        m_motionStart = canvas;
        m_motionDragging = true;
        m_gesture = m_controller.newGestureId();
        return true;
    }
    if (type == QEvent::MouseMove && m_motionDragging) {
        const QPointF position = m_motionOriginal + (canvas - m_motionStart);
        commitPosition(QPointF(std::round(position.x() * 10.0) / 10.0, std::round(position.y() * 10.0) / 10.0),
                       m_motionKey ? *m_motionKey : motionSourceTime());
        m_view.updateOverlay();
        return true;
    }
    if (type == QEvent::MouseButtonRelease && m_motionDragging) {
        m_motionDragging = false;
        m_motionKey.reset();
        m_gesture = 0;
        return true;
    }
    return false;
}

void MaskOverlay::paint(QPainter& p, const QRectF& picture, const QSizeF& canvas) {
    const auto m = layerToCanvasNow();
    if (!m || canvas.width() <= 0) {
        if (!m_pen && m_motionClip && canvas.width() > 0) {
            paintMotion(p, picture, canvas);
        }
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
        return;
    }
    if (m_motionClip) {
        paintMotion(p, picture, canvas);
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

bool MaskOverlay::mouse(int type, Qt::MouseButton button, const QPointF& canvas, double scale,
                        Qt::KeyboardModifiers modifiers) {
    if (!m_pen && mask() == nullptr) { // no mask being edited: Motion
        return motionMouse(type, button, canvas, scale, modifiers.testFlag(Qt::AltModifier));
    }
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

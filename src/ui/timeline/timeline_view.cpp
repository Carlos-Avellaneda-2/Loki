#include "ui/timeline/timeline_view.h"

#include "commands/project_commands.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/project_item_model.h"
#include "ui/sequence_edits.h"
#include "util/timecode.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QMenu>
#include <QMimeData>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace loki::ui::timeline {

namespace {

const QColor kBackground(0x1e, 0x1e, 0x1e);
const QColor kRowA(0x26, 0x26, 0x26);
const QColor kRowB(0x23, 0x23, 0x23);
const QColor kHeader(0x2b, 0x2b, 0x2b);
const QColor kRuler(0x1b, 0x1b, 0x1b);
const QColor kVideoClip(0x6c, 0x6a, 0xc4);
const QColor kAudioClip(0x3f, 0x8f, 0x63);
const QColor kAccent(0x2d, 0x8c, 0xeb);
const QColor kText(0xdd, 0xdd, 0xdd);
const QColor kDimText(0x8a, 0x8a, 0x8a);
constexpr int kButton = 18;

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// Header button rectangles of a row: lock, then visibility (eye / mute).
QRect lockRect(const TrackRow& row) {
    return {TimelineView::kHeaderWidth - 2 * (kButton + 6), row.top + (row.height - kButton) / 2, kButton, kButton};
}
QRect visibilityRect(const TrackRow& row) {
    return {TimelineView::kHeaderWidth - (kButton + 6), row.top + (row.height - kButton) / 2, kButton, kButton};
}
QRect targetRect(const TrackRow& row) {
    return {4, row.top + (row.height - kButton) / 2, kButton + 4, kButton};
}

} // namespace

TimelineView::TimelineView(ProjectController& controller, MediaServices& services, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_services(services) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumHeight(120);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent&) {
        // Drop selected clips that no longer exist (deleted, undone).
        const auto removed = std::erase_if(
            m_selection, [this](model::ClipId id) { return m_controller.project().findClip(id).clip == nullptr; });
        if (removed > 0) {
            emit selectionChanged();
        }
        update();
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] {
        m_selection.clear();
        emit selectionChanged();
        update();
    });
    connect(&m_services, &MediaServices::thumbnailReady, this, [this] { update(); });
    connect(&m_services, &MediaServices::waveformReady, this, [this] { update(); });
}

void TimelineView::setSequence(model::SequenceId id) {
    m_sequence = id;
    m_selection.clear();
        emit selectionChanged();
    m_targetVideo = {};
    m_targetAudio = {};
    if (const auto* seq = sequence()) {
        if (!seq->videoTracks.empty()) {
            m_targetVideo = seq->videoTracks.front().id;
        }
        if (!seq->audioTracks.empty()) {
            m_targetAudio = seq->audioTracks.front().id;
        }
    }
    update();
}

const model::Sequence* TimelineView::sequence() const {
    return m_controller.project().findSequence(m_sequence);
}

void TimelineView::setTool(Tool tool) {
    if (tool != m_tool) {
        m_tool = tool;
        setCursor(tool == Tool::Razor ? Qt::CrossCursor : Qt::ArrowCursor);
        emit toolChanged(tool);
    }
}

void TimelineView::setSnapping(bool on) {
    if (on != m_snapping) {
        m_snapping = on;
        emit snappingChanged(on);
    }
}

void TimelineView::setSelection(std::set<model::ClipId> clips) {
    m_selection = std::move(clips);
    emit selectionChanged();
    update();
}

commands::EditTargets TimelineView::targets() const {
    return {m_targetVideo, m_targetAudio};
}

void TimelineView::setTargets(model::TrackId video, model::TrackId audio) {
    m_targetVideo = video;
    m_targetAudio = audio;
    update();
}

std::vector<TrackRow> TimelineView::rows() const {
    const auto* seq = sequence();
    return seq != nullptr ? layoutTracks(*seq, m_metrics) : std::vector<TrackRow>{};
}

Rational TimelineView::playhead() const {
    return m_transport.position ? m_transport.position() : Rational();
}

Rational TimelineView::frameRate() const {
    const auto* seq = sequence();
    return seq != nullptr ? seq->settings.frameRate : Rational(30);
}

Rational TimelineView::timeAt(double widgetX) const {
    return xToTime(contentX(widgetX), m_metrics, frameRate());
}

Rational TimelineView::snapped(const Rational& t, const std::set<model::ClipId>& exclude) const {
    const auto* seq = sequence();
    if (!m_snapping || seq == nullptr) {
        return t;
    }
    return snapTime(t, snapCandidates(*seq, playhead(), exclude), m_metrics).value_or(t);
}

const model::Track* TimelineView::trackOf(const TrackRow& row) const {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return nullptr;
    }
    const auto& list = seq->tracks(row.kind);
    return row.index < list.size() ? &list[row.index] : nullptr;
}

double TimelineView::contentWidthPixels() const {
    const auto* seq = sequence();
    const Rational end = max(seq != nullptr ? seq->duration() : Rational(), playhead()) + Rational(30);
    return end.toDouble() * m_metrics.pixelsPerSecond;
}

void TimelineView::setPixelsPerSecond(double pps, std::optional<double> anchorX) {
    pps = std::clamp(pps, 0.5, 4000.0);
    const double anchor = anchorX.value_or(timeToX(playhead(), m_metrics));
    const double anchorSeconds = (anchor + m_metrics.scrollX) / m_metrics.pixelsPerSecond;
    m_metrics.pixelsPerSecond = pps;
    m_metrics.scrollX = std::max(0.0, anchorSeconds * pps - anchor);
    emit zoomChanged(pps);
    emit scrollChanged();
    update();
}

void TimelineView::zoomToFit() {
    const auto* seq = sequence();
    const double seconds = std::max(1.0, seq != nullptr ? seq->duration().toDouble() : 1.0);
    m_metrics.scrollX = 0;
    setPixelsPerSecond((width() - kHeaderWidth - 20) / seconds, 0.0);
}

void TimelineView::scrollToTime(const Rational& t) {
    const double x = timeToX(t, m_metrics);
    const double visible = width() - kHeaderWidth;
    if (x < 0 || x > visible) {
        m_metrics.scrollX = std::max(0.0, t.toDouble() * m_metrics.pixelsPerSecond - visible * 0.1);
        emit scrollChanged();
        update();
    }
}

void TimelineView::refreshPlayhead() {
    const Rational now = playhead();
    if (now == m_lastPlayhead) {
        return;
    }
    m_lastPlayhead = now;
    if (m_transport.isPlaying && m_transport.isPlaying()) {
        // Page-follow during playback, like Premiere.
        const double x = timeToX(now, m_metrics);
        const double visible = width() - kHeaderWidth;
        if (x > visible || x < 0) {
            m_metrics.scrollX = std::max(0.0, now.toDouble() * m_metrics.pixelsPerSecond);
            emit scrollChanged();
        }
    }
    update();
}

// --- Painting ----------------------------------------------------------------------------------

void TimelineView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    const auto* seq = sequence();
    if (seq == nullptr) {
        p.setPen(kDimText);
        p.drawText(rect(), Qt::AlignCenter, tr("No sequence"));
        return;
    }
    const auto rowList = rows();
    const QRect content(kHeaderWidth, m_metrics.rulerHeight, width() - kHeaderWidth, height() - m_metrics.rulerHeight);

    p.save();
    p.setClipRect(content);
    for (const auto& row : rowList) {
        p.fillRect(QRect(kHeaderWidth, row.top, content.width(), row.height),
                   (row.index % 2 == 0) ? kRowA : kRowB);
        if (const auto* track = trackOf(row)) {
            paintTrackClips(p, row, *track);
        }
        p.setPen(QColor(0x18, 0x18, 0x18));
        p.drawLine(kHeaderWidth, row.top + row.height - 1, width(), row.top + row.height - 1);
    }
    paintGhosts(p, rowList);
    if (seq->markIn || seq->markOut) {
        const double x0 = kHeaderWidth + timeToX(seq->markIn.value_or(Rational()), m_metrics);
        const double x1 = kHeaderWidth + timeToX(seq->markOut.value_or(seq->duration()), m_metrics);
        p.fillRect(QRectF(x0, content.top(), std::max(1.0, x1 - x0), content.height()), QColor(0x2d, 0x8c, 0xeb, 28));
    }
    if (m_snapIndicator) {
        const double x = kHeaderWidth + timeToX(*m_snapIndicator, m_metrics);
        p.setPen(QPen(QColor(0xff, 0xd0, 0x40), 1, Qt::DashLine));
        p.drawLine(QPointF(x, content.top()), QPointF(x, content.bottom()));
    }
    p.restore();

    paintHeaders(p, rowList);
    paintRuler(p, QRect(kHeaderWidth, 0, width() - kHeaderWidth, m_metrics.rulerHeight));
    if (seq->markIn || seq->markOut) {
        // In/Out band on the ruler.
        const double x0 = std::max<double>(kHeaderWidth, kHeaderWidth + timeToX(seq->markIn.value_or(Rational()), m_metrics));
        const double x1 = kHeaderWidth + timeToX(seq->markOut.value_or(seq->duration()), m_metrics);
        if (x1 > x0) {
            p.fillRect(QRectF(x0, m_metrics.rulerHeight - 6, x1 - x0, 5), QColor(0x2d, 0x8c, 0xeb, 160));
        }
    }

    // Header corner: playhead timecode.
    const QRect corner(0, 0, kHeaderWidth, m_metrics.rulerHeight);
    p.fillRect(corner, kRuler);
    p.setPen(kAccent);
    QFont mono(QStringLiteral("Consolas"));
    mono.setPointSizeF(10.5);
    p.setFont(mono);
    const auto rate = frameRate();
    p.drawText(corner.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
               qs(formatTimecode(timeToFrames(playhead(), rate, Rounding::Floor), rate, supportsDropFrame(rate))));

    // Playhead line across ruler and tracks.
    const double x = kHeaderWidth + timeToX(playhead(), m_metrics);
    if (x >= kHeaderWidth && x <= width()) {
        p.setPen(QPen(kAccent, 1));
        p.drawLine(QPointF(x, m_metrics.rulerHeight - 8), QPointF(x, height()));
        p.setBrush(kAccent);
        p.setPen(Qt::NoPen);
        const QPointF head[] = {QPointF(x - 6, m_metrics.rulerHeight - 12), QPointF(x + 6, m_metrics.rulerHeight - 12),
                                QPointF(x, m_metrics.rulerHeight - 4)};
        p.drawPolygon(head, 3);
    }
}

void TimelineView::paintRuler(QPainter& p, const QRect& area) const {
    p.fillRect(area, kRuler);
    const auto rate = frameRate();
    const Rational step = rulerStep(m_metrics, rate);
    const double firstSecond = m_metrics.scrollX / m_metrics.pixelsPerSecond;
    auto index = static_cast<std::int64_t>(std::floor(firstSecond / step.toDouble()));
    QFont font = p.font();
    font.setPointSizeF(8.0);
    p.setFont(font);
    for (;; ++index) {
        const Rational t = step * Rational(index);
        const double x = area.left() + timeToX(t, m_metrics);
        if (x > area.right()) {
            break;
        }
        // Minor ticks between labels.
        for (int k = 1; k < 4; ++k) {
            const double mx = x + step.toDouble() * m_metrics.pixelsPerSecond * k / 4.0;
            if (mx >= area.left() && mx <= area.right()) {
                p.setPen(QColor(0x50, 0x50, 0x50));
                p.drawLine(QPointF(mx, area.bottom() - 4), QPointF(mx, area.bottom()));
            }
        }
        if (x < area.left()) {
            continue;
        }
        p.setPen(QColor(0x70, 0x70, 0x70));
        p.drawLine(QPointF(x, area.bottom() - 10), QPointF(x, area.bottom()));
        p.setPen(kDimText);
        p.drawText(QPointF(x + 3, area.top() + 12),
                   qs(formatTimecode(timeToFrames(t, rate, Rounding::Floor), rate, supportsDropFrame(rate))));
    }
}

void TimelineView::paintHeaders(QPainter& p, const std::vector<TrackRow>& rowList) const {
    p.save();
    p.setClipRect(QRect(0, m_metrics.rulerHeight, kHeaderWidth, height()));
    p.fillRect(QRect(0, m_metrics.rulerHeight, kHeaderWidth, height()), kBackground);
    for (const auto& row : rowList) {
        const auto* track = trackOf(row);
        if (track == nullptr) {
            continue;
        }
        const QRect r(0, row.top, kHeaderWidth - 1, row.height - 1);
        p.fillRect(r, kHeader);
        // Target (source patching) indicator.
        const bool targeted = track->id == m_targetVideo || track->id == m_targetAudio;
        const QRect target = targetRect(row);
        p.fillRect(target, targeted ? kAccent : QColor(0x3a, 0x3a, 0x3a));
        p.setPen(targeted ? Qt::white : kDimText);
        p.drawText(target, Qt::AlignCenter, row.kind == model::TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A"));
        // Name.
        p.setPen(kText);
        p.drawText(QRect(target.right() + 8, row.top, 60, row.height), Qt::AlignVCenter | Qt::AlignLeft,
                   qs(track->state.name));
        // Lock.
        const QRect lock = lockRect(row);
        p.fillRect(lock, track->state.locked ? QColor(0xc8, 0x8a, 0x2a) : QColor(0x3a, 0x3a, 0x3a));
        p.setPen(track->state.locked ? Qt::white : kDimText);
        p.drawText(lock, Qt::AlignCenter, QStringLiteral("L"));
        // Visibility (video eye) / mute (audio).
        const QRect vis = visibilityRect(row);
        const bool off = row.kind == model::TrackKind::Video ? track->state.hidden : track->state.muted;
        p.fillRect(vis, off ? QColor(0xb0, 0x3a, 0x3a) : QColor(0x3a, 0x3a, 0x3a));
        p.setPen(off ? Qt::white : kDimText);
        p.drawText(vis, Qt::AlignCenter, row.kind == model::TrackKind::Video ? QStringLiteral("E") : QStringLiteral("M"));
    }
    p.restore();
}

void TimelineView::paintTrackClips(QPainter& p, const TrackRow& row, const model::Track& track) const {
    const double visibleStart = m_metrics.scrollX / m_metrics.pixelsPerSecond;
    const double visibleEnd = visibleStart + (width() - kHeaderWidth) / m_metrics.pixelsPerSecond;
    for (const auto& clip : track.clips) {
        if (clip.end().toDouble() < visibleStart || clip.start.toDouble() > visibleEnd) {
            continue; // off screen: never painted
        }
        const double x0 = kHeaderWidth + timeToX(clip.start, m_metrics);
        const double x1 = kHeaderWidth + timeToX(clip.end(), m_metrics);
        const QRectF r(x0, row.top + 2, std::max(1.0, x1 - x0 - 1), row.height - 5);
        const bool dragging = (m_drag == Drag::Move || m_drag == Drag::Slide || m_drag == Drag::TrimIn ||
                               m_drag == Drag::TrimOut) &&
                              m_selection.contains(clip.id);
        paintClip(p, r, clip, track.kind, m_selection.contains(clip.id));
        if (dragging) {
            p.fillRect(r, QColor(0, 0, 0, 90)); // original position dimmed while the ghost moves
        }
    }
    if (track.state.locked) {
        p.fillRect(QRect(kHeaderWidth, row.top, width() - kHeaderWidth, row.height),
                   QBrush(QColor(255, 255, 255, 18), Qt::BDiagPattern));
    }
}

void TimelineView::paintClip(QPainter& p, const QRectF& r, const model::Clip& clip, model::TrackKind kind,
                             bool selected) const {
    QColor base = kind == model::TrackKind::Video ? kVideoClip : kAudioClip;
    if (!clip.enabled) {
        base = base.darker(180);
    }
    if (selected) {
        base = base.lighter(130);
    }
    p.fillRect(r, base);
    const auto* media = m_controller.project().findMedia(clip.media);

    QRectF body = r;
    if (kind == model::TrackKind::Video && media != nullptr && r.width() > 40) {
        // Head thumbnail.
        const QImage thumb = m_services.thumbnail(*media);
        if (!thumb.isNull()) {
            const double h = r.height() - 14;
            const double w = std::min(r.width() / 2, h * thumb.width() / std::max(1, thumb.height()));
            p.drawImage(QRectF(r.left() + 1, r.top() + 13, w, h), thumb);
        }
    }
    if (kind == model::TrackKind::Audio && media != nullptr) {
        paintWaveform(p, body.adjusted(1, 12, -1, -1), clip);
    }
    // Name bar.
    if (r.width() > 12) {
        p.setPen(selected ? Qt::white : kText);
        QFont f = p.font();
        f.setPointSizeF(8.0);
        p.setFont(f);
        const QString name = qs(clip.name.empty() && media != nullptr ? media->name : clip.name);
        p.drawText(r.adjusted(4, 1, -2, 0), Qt::AlignLeft | Qt::AlignTop, name);
    }
    if (selected) {
        p.setPen(QPen(Qt::white, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

void TimelineView::paintWaveform(QPainter& p, const QRectF& r, const model::Clip& clip) const {
    const auto* media = m_controller.project().findMedia(clip.media);
    if (media == nullptr) {
        return;
    }
    const auto wf = m_services.waveform(*media);
    if (!wf || wf->peakCount() == 0 || r.width() < 2) {
        return;
    }
    const double mid = r.center().y();
    const double half = r.height() / 2.0 * 0.95;
    const double secondsPerPixel = 1.0 / m_metrics.pixelsPerSecond;
    const double peaksPerSecond = static_cast<double>(wf->sampleRate) / wf->samplesPerPeak;
    const double sourceStart = clip.sourceIn.toDouble();
    // Visible part only.
    const int xFrom = static_cast<int>(std::max(r.left(), static_cast<double>(kHeaderWidth)));
    const int xTo = static_cast<int>(std::min(r.right(), static_cast<double>(width())));
    p.setPen(QColor(0xc8, 0xf0, 0xd8, 200));
    for (int x = xFrom; x < xTo; ++x) {
        const double s0 = sourceStart + (x - r.left()) * secondsPerPixel;
        const double s1 = s0 + secondsPerPixel;
        auto p0 = static_cast<std::size_t>(std::max(0.0, s0 * peaksPerSecond));
        auto p1 = static_cast<std::size_t>(std::max(0.0, s1 * peaksPerSecond));
        p1 = std::max(p1, p0 + 1);
        if (p0 >= wf->peakCount()) {
            break;
        }
        int lo = 0;
        int hi = 0;
        for (auto i = p0; i < std::min(p1, wf->peakCount()); ++i) {
            for (int c = 0; c < wf->channels; ++c) {
                lo = std::min<int>(lo, wf->min(i, c));
                hi = std::max<int>(hi, wf->max(i, c));
            }
        }
        p.drawLine(QPointF(x, mid - hi / 32768.0 * half), QPointF(x, mid - lo / 32768.0 * half));
    }
}

void TimelineView::paintGhosts(QPainter& p, const std::vector<TrackRow>& rowList) const {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    p.setPen(QPen(Qt::white, 1, Qt::DashLine));
    p.setBrush(QColor(255, 255, 255, 40));
    if (m_drag == Drag::Move || m_drag == Drag::Slide || m_drag == Drag::TrimIn || m_drag == Drag::TrimOut) {
        for (const auto& row : rowList) {
            const auto* track = trackOf(row);
            for (const auto& clip : track->clips) {
                if (!m_selection.contains(clip.id)) {
                    continue;
                }
                Rational start = clip.start;
                Rational end = clip.end();
                const TrackRow* dest = &row;
                if (m_drag == Drag::Slide) {
                    start += m_dragDelta;
                    end += m_dragDelta;
                } else if (m_drag == Drag::Move) {
                    start += m_dragDelta;
                    end += m_dragDelta;
                    if (row.kind == m_dragKind && m_dragTrackDelta != 0) {
                        const auto target = static_cast<std::ptrdiff_t>(row.index) + m_dragTrackDelta;
                        dest = nullptr;
                        for (const auto& r : rowList) {
                            if (r.kind == row.kind && static_cast<std::ptrdiff_t>(r.index) == target) {
                                dest = &r;
                            }
                        }
                    }
                } else if (m_drag == Drag::TrimIn && (clip.id == m_dragClip || m_dragLinked)) {
                    start += m_dragDelta;
                } else if (m_drag == Drag::TrimOut && (clip.id == m_dragClip || m_dragLinked)) {
                    end += m_dragDelta;
                }
                if (dest == nullptr) {
                    continue;
                }
                const double x0 = kHeaderWidth + timeToX(start, m_metrics);
                const double x1 = kHeaderWidth + timeToX(end, m_metrics);
                p.drawRect(QRectF(x0, dest->top + 2, std::max(1.0, x1 - x0 - 1), dest->height - 5));
            }
        }
    }
    if (m_dropPreview && m_dropPreview->row) {
        const auto& row = *m_dropPreview->row;
        const double x0 = kHeaderWidth + timeToX(m_dropPreview->at, m_metrics);
        const double x1 = kHeaderWidth + timeToX(m_dropPreview->at + m_dropPreview->duration, m_metrics);
        p.drawRect(QRectF(x0, row.top + 2, std::max(1.0, x1 - x0 - 1), row.height - 5));
    }
}

// --- Interaction -------------------------------------------------------------------------------

void TimelineView::runEdit(const std::function<std::unique_ptr<commands::Command>()>& make) {
    try {
        if (auto command = make()) {
            m_controller.execute(std::move(command));
        }
    } catch (const std::exception& e) {
        emit statusMessage(QString::fromUtf8(e.what()));
    }
}

void TimelineView::selectWithLinked(model::ClipId clip, bool toggle, bool add) {
    std::vector<model::ClipId> group{clip};
    try {
        commands::EditPlan plan(m_controller.project(), m_sequence);
        group = plan.withLinked(clip);
    } catch (const std::exception&) {
    }
    const bool selected = m_selection.contains(clip);
    if (toggle && selected) {
        for (const auto id : group) {
            m_selection.erase(id);
        }
    } else {
        if (!toggle && !add && !selected) {
            m_selection.clear();
        emit selectionChanged();
        }
        m_selection.insert(group.begin(), group.end());
    }
    emit selectionChanged();
    update();
}

void TimelineView::headerClick(const TrackRow& row, const QPoint& pos) {
    const auto* track = trackOf(row);
    if (track == nullptr) {
        return;
    }
    const auto& project = m_controller.project();
    if (lockRect(row).contains(pos)) {
        runEdit([&] { return commands::setTrackLocked(project, track->id, !track->state.locked); });
    } else if (visibilityRect(row).contains(pos)) {
        runEdit([&] {
            return row.kind == model::TrackKind::Video ? commands::setTrackHidden(project, track->id, !track->state.hidden)
                                                       : commands::setTrackMuted(project, track->id, !track->state.muted);
        });
    } else if (targetRect(row).contains(pos)) {
        // Toggle source patching for this kind of track.
        auto& target = row.kind == model::TrackKind::Video ? m_targetVideo : m_targetAudio;
        target = target == track->id ? model::TrackId() : track->id;
        update();
    }
}

void TimelineView::mousePressEvent(QMouseEvent* event) {
    setFocus();
    const QPoint pos = event->position().toPoint();
    const auto* seq = sequence();
    if (seq == nullptr || event->button() != Qt::LeftButton) {
        return;
    }
    if (pos.y() < m_metrics.rulerHeight) {
        if (pos.x() >= kHeaderWidth) {
            m_drag = Drag::Scrub;
            if (m_transport.seek) {
                m_transport.seek(timeAt(pos.x()));
            }
        }
        return;
    }
    const auto rowList = rows();
    const auto* row = rowAt(rowList, pos.y());
    if (row == nullptr) {
        return;
    }
    if (pos.x() < kHeaderWidth) {
        headerClick(*row, pos);
        return;
    }
    const auto hit = hitTest(*seq, rowList, m_metrics, contentX(pos.x()), pos.y());
    if (m_tool == Tool::Razor) {
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            const Rational at = snapped(timeAt(pos.x()));
            runEdit([&] { return commands::makeRazorAllTracks(m_controller.project(), m_sequence, at); });
        } else if (hit.clip) {
            const Rational at = snapped(timeAt(pos.x()));
            runEdit([&] { return commands::makeRazorEdit(m_controller.project(), m_sequence, {hit.clip}, at); });
        }
        return;
    }
    if (!hit.clip) {
        if (!event->modifiers().testFlag(Qt::ControlModifier)) {
            m_selection.clear();
            emit selectionChanged();
            update();
        }
        return;
    }
    if (m_tool == Tool::Slip || m_tool == Tool::Slide) {
        selectWithLinked(hit.clip, false, false);
        m_drag = m_tool == Tool::Slip ? Drag::Slip : Drag::Slide;
        m_dragStart = pos;
        m_dragClip = hit.clip;
        m_dragKind = row->kind;
        m_dragRowIndex = row->index;
        m_dragDelta = Rational();
        m_dragTrackDelta = 0;
        m_dragLinked = !event->modifiers().testFlag(Qt::AltModifier);
        return;
    }
    if ((m_tool == Tool::Ripple || m_tool == Tool::Roll) && hit.zone == HitZone::Body) {
        selectWithLinked(hit.clip, false, false);
        return;
    }
    const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
    const bool add = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool unlink = event->modifiers().testFlag(Qt::AltModifier);
    if (hit.zone == HitZone::Body) {
        if (unlink) {
            if (!toggle && !add) {
                m_selection.clear();
        emit selectionChanged();
            }
            m_selection.insert(hit.clip);
            emit selectionChanged();
            update();
        } else {
            selectWithLinked(hit.clip, toggle, add);
        }
        if (!m_selection.contains(hit.clip)) {
            return; // toggled off
        }
        m_drag = Drag::Move;
    } else {
        if (!m_selection.contains(hit.clip)) {
            m_selection.clear();
        emit selectionChanged();
            selectWithLinked(hit.clip, false, false);
        }
        m_drag = hit.zone == HitZone::InEdge ? Drag::TrimIn : Drag::TrimOut;
    }
    m_dragStart = pos;
    m_dragClip = hit.clip;
    m_dragKind = row->kind;
    m_dragRowIndex = row->index;
    m_dragDelta = Rational();
    m_dragTrackDelta = 0;
    m_dragLinked = !unlink;
}

void TimelineView::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    const auto* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    switch (m_drag) {
    case Drag::None: updateHoverCursor(pos); return;
    case Drag::Scrub:
        if (m_transport.seek) {
            m_transport.seek(snapped(timeAt(pos.x())));
        }
        return;
    case Drag::Move: {
        Rational delta = timeAt(pos.x()) - timeAt(m_dragStart.x());
        // Block of selected clips: snap either edge, and never before 0.
        Rational blockStart;
        Rational blockEnd;
        bool first = true;
        for (const auto* list : {&seq->videoTracks, &seq->audioTracks}) {
            for (const auto& t : *list) {
                for (const auto& c : t.clips) {
                    if (m_selection.contains(c.id)) {
                        blockStart = first ? c.start : min(blockStart, c.start);
                        blockEnd = first ? c.end() : max(blockEnd, c.end());
                        first = false;
                    }
                }
            }
        }
        m_snapIndicator.reset();
        if (m_snapping && !first) {
            const auto candidates = snapCandidates(*seq, playhead(), m_selection);
            if (const auto adjust = snapBlock(blockStart + delta, blockEnd + delta, candidates, m_metrics)) {
                delta += *adjust;
                const auto edgeStart = blockStart + delta;
                m_snapIndicator = std::find(candidates.begin(), candidates.end(), edgeStart) != candidates.end()
                                      ? edgeStart
                                      : blockEnd + delta;
            }
        }
        if (!first && (blockStart + delta).isNegative()) {
            delta = -blockStart;
        }
        m_dragDelta = delta;
        const auto rowList = rows();
        if (const auto* row = rowAt(rowList, pos.y()); row != nullptr && row->kind == m_dragKind) {
            m_dragTrackDelta = static_cast<int>(row->index) - static_cast<int>(m_dragRowIndex);
        }
        update();
        return;
    }
    case Drag::Slip:
    case Drag::Slide: {
        m_dragDelta = timeAt(pos.x()) - timeAt(m_dragStart.x());
        if (m_drag == Drag::Slip) {
            const auto rate = frameRate();
            emit statusMessage(tr("Slip %1 frames").arg(-timeToFrames(m_dragDelta, rate, Rounding::Nearest)));
        }
        update();
        return;
    }
    case Drag::TrimIn:
    case Drag::TrimOut: {
        const auto ref = m_controller.project().findClip(m_dragClip);
        if (ref.clip == nullptr) {
            return;
        }
        const Rational edge = m_drag == Drag::TrimIn ? ref.clip->start : ref.clip->end();
        const Rational target = snapped(timeAt(pos.x()), {m_dragClip});
        m_dragDelta = target - edge;
        m_snapIndicator = target;
        update();
        return;
    }
    }
}

void TimelineView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const Drag drag = m_drag;
    m_drag = Drag::None;
    m_snapIndicator.reset();
    const auto& project = m_controller.project();
    if (drag == Drag::Move && (!m_dragDelta.isZero() || m_dragTrackDelta != 0)) {
        const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
        const auto dt = m_dragDelta;
        const auto dTrack = m_dragTrackDelta;
        const auto kind = m_dragKind;
        if (m_dragLinked) {
            runEdit([&] { return commands::makeMoveClips(project, m_sequence, ids, dt, dTrack, kind); });
        } else {
            // Alt-drag: move only the clicked clip without its linked partners.
            runEdit([&] {
                commands::EditPlan plan(project, m_sequence);
                auto placed = plan.take(m_dragClip);
                if (!placed) {
                    return std::unique_ptr<commands::Command>();
                }
                placed->clip.start += dt;
                plan.clearRange(placed->track, placed->clip.start, placed->clip.end());
                plan.add(placed->track, std::move(placed->clip));
                return plan.build("Move");
            });
        }
    } else if ((drag == Drag::TrimIn || drag == Drag::TrimOut) && !m_dragDelta.isZero()) {
        const auto edge = drag == Drag::TrimIn ? commands::ClipEdge::In : commands::ClipEdge::Out;
        const auto dt = m_dragDelta;
        runEdit([&] {
            switch (m_tool) {
            case Tool::Ripple: return commands::makeRippleTrim(project, m_sequence, m_dragClip, edge, dt, m_dragLinked);
            case Tool::Roll: return commands::makeRollEdit(project, m_sequence, m_dragClip, edge, dt, m_dragLinked);
            default: return commands::makeTrimEdge(project, m_sequence, m_dragClip, edge, dt, m_dragLinked);
            }
        });
    } else if (drag == Drag::Slip && !m_dragDelta.isZero()) {
        // Dragging right moves the content right: earlier source frames come into view.
        const auto dt = -m_dragDelta;
        runEdit([&] { return commands::makeSlipEdit(project, m_sequence, m_dragClip, dt, m_dragLinked); });
    } else if (drag == Drag::Slide && !m_dragDelta.isZero()) {
        const auto dt = m_dragDelta;
        runEdit([&] { return commands::makeSlideEdit(project, m_sequence, m_dragClip, dt, m_dragLinked); });
    }
    m_dragDelta = Rational();
    m_dragTrackDelta = 0;
    update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-click on the ruler: move the playhead there (same as a click).
    mousePressEvent(event);
}

void TimelineView::updateHoverCursor(const QPoint& pos) {
    const auto* seq = sequence();
    if (seq == nullptr || m_tool == Tool::Razor) {
        return;
    }
    if (pos.x() < kHeaderWidth || pos.y() < m_metrics.rulerHeight) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    const auto hit = hitTest(*seq, rows(), m_metrics, contentX(pos.x()), pos.y());
    setCursor(hit.zone == HitZone::InEdge || hit.zone == HitZone::OutEdge ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void TimelineView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        const double anchor = contentX(event->position().x());
        setPixelsPerSecond(m_metrics.pixelsPerSecond * std::pow(1.25, steps), anchor);
    } else if (event->modifiers() & Qt::ShiftModifier) {
        const auto* seq = sequence();
        const int maxY = std::max(0, (seq != nullptr ? contentHeight(*seq, m_metrics) : 0) - height());
        m_metrics.scrollY = std::clamp(m_metrics.scrollY - static_cast<int>(steps * 40), 0, maxY);
        emit scrollChanged();
        update();
    } else {
        m_metrics.scrollX = std::max(0.0, m_metrics.scrollX - steps * 80.0);
        emit scrollChanged();
        update();
    }
    event->accept();
}

void TimelineView::deleteSelection() {
    if (m_selection.empty()) {
        return;
    }
    const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
    runEdit([&] { return commands::makeDeleteClips(m_controller.project(), m_sequence, ids, false); });
}

void TimelineView::razorAtPlayhead(bool allTracks) {
    const Rational at = playhead();
    if (allTracks || m_selection.empty()) {
        runEdit([&] { return commands::makeRazorAllTracks(m_controller.project(), m_sequence, at); });
    } else {
        const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
        runEdit([&] { return commands::makeRazorEdit(m_controller.project(), m_sequence, ids, at); });
    }
}

void TimelineView::rippleDeleteSelection() {
    if (m_selection.empty()) {
        return;
    }
    const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
    runEdit([&] { return commands::makeRippleDelete(m_controller.project(), m_sequence, ids); });
}

void TimelineView::toggleLinkSelection() {
    if (m_selection.empty()) {
        return;
    }
    const auto& project = m_controller.project();
    const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
    // Link when the selection is not already one group; unlink otherwise (Premiere's Ctrl+L).
    model::LinkId common;
    bool oneGroup = true;
    for (const auto id : ids) {
        const auto* clip = project.findClip(id).clip;
        if (clip == nullptr || clip->link.isNull() || (common && clip->link != common)) {
            oneGroup = false;
            break;
        }
        common = clip->link;
    }
    runEdit([&] {
        return oneGroup ? commands::makeUnlinkClips(project, m_sequence, ids)
                        : commands::makeLinkClips(project, m_sequence, ids);
    });
}

void TimelineView::markIn() {
    sequence_edits::markIn(m_controller, m_sequence, playhead());
}

void TimelineView::markOut() {
    sequence_edits::markOut(m_controller, m_sequence, playhead());
}

void TimelineView::clearMarks() {
    sequence_edits::clearMarks(m_controller, m_sequence);
}

void TimelineView::goToIn() {
    if (const auto in = sequence_edits::markInOf(m_controller, m_sequence); in && m_transport.seek) {
        m_transport.seek(*in);
        scrollToTime(*in);
    }
}

void TimelineView::goToOut() {
    if (const auto out = sequence_edits::markOutFrameOf(m_controller, m_sequence); out && m_transport.seek) {
        m_transport.seek(*out);
        scrollToTime(*out);
    }
}

void TimelineView::lift() {
    sequence_edits::lift(m_controller, m_sequence);
}

void TimelineView::extract() {
    sequence_edits::extract(m_controller, m_sequence);
}

void TimelineView::goToEditPoint(bool next) {
    const auto* seq = sequence();
    if (seq == nullptr || !m_transport.seek) {
        return;
    }
    const auto point = next ? nextEditPoint(*seq, playhead()) : previousEditPoint(*seq, playhead());
    if (point) {
        m_transport.seek(*point);
        scrollToTime(*point);
    }
}

void TimelineView::stepPlayhead(int frames) {
    if (m_transport.step) {
        m_transport.step(frames);
    }
}

void TimelineView::togglePlay() {
    if (m_transport.togglePlay) {
        m_transport.togglePlay();
    }
}

void TimelineView::goToStart() {
    if (m_transport.seek) {
        m_transport.seek(Rational());
        scrollToTime(Rational());
    }
}

void TimelineView::goToEnd() {
    const auto* seq = sequence();
    if (seq != nullptr && m_transport.seek) {
        m_transport.seek(seq->duration());
        scrollToTime(seq->duration());
    }
}

void TimelineView::contextMenuEvent(QContextMenuEvent* event) {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    QMenu menu(this);
    const auto rowList = rows();
    const auto* row = rowAt(rowList, event->pos().y());
    const auto& project = m_controller.project();
    if (event->pos().x() < kHeaderWidth) {
        menu.addAction(tr("Add Video Track"), this, [this] {
            runEdit([&] { return std::make_unique<commands::AddTrackCommand>(m_sequence, model::TrackKind::Video); });
        });
        menu.addAction(tr("Add Audio Track"), this, [this] {
            runEdit([&] { return std::make_unique<commands::AddTrackCommand>(m_sequence, model::TrackKind::Audio); });
        });
        if (row != nullptr) {
            const auto trackId = row->id;
            menu.addSeparator();
            menu.addAction(tr("Delete Track"), this, [this, trackId] {
                runEdit([&] { return std::make_unique<commands::RemoveTrackCommand>(trackId); });
            });
        }
    } else {
        const auto hit = row != nullptr ? hitTest(*seq, rowList, m_metrics, contentX(event->pos().x()), event->pos().y())
                                        : Hit{};
        if (hit.clip && !m_selection.contains(hit.clip)) {
            selectWithLinked(hit.clip, false, false);
        }
        if (!m_selection.empty()) {
            const std::vector<model::ClipId> ids(m_selection.begin(), m_selection.end());
            const auto* clip = project.findClip(ids.front()).clip;
            const bool enable = clip != nullptr && !clip->enabled;
            menu.addAction(enable ? tr("Enable") : tr("Disable"), this, [this, ids, enable] {
                std::vector<std::unique_ptr<commands::Command>> edits;
                for (const auto id : ids) {
                    edits.push_back(commands::setClipEnabled(id, enable));
                }
                runEdit([&] {
                    return std::make_unique<commands::MacroCommand>(enable ? "Enable" : "Disable", std::move(edits));
                });
            });
            menu.addAction(tr("Delete"), this, [this] { deleteSelection(); });
            menu.addAction(tr("Ripple Delete"), this, [this] { rippleDeleteSelection(); });
            menu.addSeparator();
            menu.addAction(tr("Link / Unlink"), this, [this] { toggleLinkSelection(); });
        }
    }
    if (!menu.isEmpty()) {
        menu.exec(event->globalPos());
    }
}

// --- Drag & drop from the Project panel ----------------------------------------------------------

std::optional<TimelineView::DropPreview> TimelineView::dropPreviewAt(const QMimeData* mime, const QPoint& pos) const {
    const auto ids = ProjectItemModel::decodeMediaIds(mime);
    if (ids.empty() || pos.x() < kHeaderWidth) {
        return std::nullopt;
    }
    const auto* media = m_controller.project().findMedia(ids.front());
    if (media == nullptr) {
        return std::nullopt;
    }
    const auto rowList = rows();
    const auto* row = rowAt(rowList, pos.y());
    DropPreview preview;
    preview.media = media->id;
    preview.duration = commands::sourceRangeOf(*media).duration;
    preview.at = snapped(timeAt(pos.x()));
    if (row != nullptr) {
        preview.row = *row;
    }
    return preview;
}

void TimelineView::dragEnterEvent(QDragEnterEvent* event) {
    if (!ProjectItemModel::decodeMediaIds(event->mimeData()).empty()) {
        event->acceptProposedAction();
    }
}

void TimelineView::dragMoveEvent(QDragMoveEvent* event) {
    m_dropPreview = dropPreviewAt(event->mimeData(), event->position().toPoint());
    if (m_dropPreview) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
    update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*) {
    m_dropPreview.reset();
    update();
}

void TimelineView::dropEvent(QDropEvent* event) {
    auto preview = dropPreviewAt(event->mimeData(), event->position().toPoint());
    m_dropPreview.reset();
    update();
    if (!preview || !preview->row) {
        return;
    }
    const auto* seq = sequence();
    const auto* media = m_controller.project().findMedia(preview->media);
    if (seq == nullptr || media == nullptr) {
        return;
    }
    // Dropped on Vn: video on Vn and audio on An (or A1); dropped on An: the mirror.
    const auto& row = *preview->row;
    commands::EditTargets targets;
    const auto pick = [&](model::TrackKind kind) -> model::TrackId {
        const auto& list = seq->tracks(kind);
        if (list.empty()) {
            return {};
        }
        return row.index < list.size() ? list[row.index].id : list.front().id;
    };
    targets.video = row.kind == model::TrackKind::Video ? row.id : pick(model::TrackKind::Video);
    targets.audio = row.kind == model::TrackKind::Audio ? row.id : pick(model::TrackKind::Audio);
    const auto range = commands::sourceRangeOf(*media);
    const bool insert = event->modifiers().testFlag(Qt::ControlModifier);
    runEdit([&] {
        return insert ? commands::makeInsertEdit(m_controller.project(), m_sequence, range, preview->at, targets)
                      : commands::makeOverwriteEdit(m_controller.project(), m_sequence, range, preview->at, targets);
    });
    event->acceptProposedAction();
}

} // namespace loki::ui::timeline

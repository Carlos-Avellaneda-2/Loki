#pragma once

#include "commands/clip_commands.h"
#include "commands/edit_commands.h"
#include "model/types.h"
#include "ui/timeline/timeline_geometry.h"

#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <set>

class QMimeData;

namespace loki::ui {
class MediaServices;
class ProjectController;
} // namespace loki::ui

namespace loki::ui::timeline {

enum class Tool { Selection, Razor, Ripple, Roll, Slip, Slide };

// Connection to the Program Monitor's playback engine (the playhead lives there).
struct Transport {
    std::function<Rational()> position;
    std::function<void(const Rational&)> seek;
    std::function<void()> togglePlay;
    std::function<void(int)> step;
    std::function<bool()> isPlaying;
};

// Timeline editing surface: track headers (left), ruler (top), clips, playhead. Painting covers
// only the visible range. Edits go through commands; drags show a ghost and commit on release so
// one gesture is one undo step and an abandoned drag changes nothing.
class TimelineView final : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeaderWidth = 150;

    TimelineView(ProjectController& controller, MediaServices& services, QWidget* parent = nullptr);

    void setSequence(model::SequenceId id);
    [[nodiscard]] model::SequenceId sequenceId() const noexcept { return m_sequence; }
    [[nodiscard]] const model::Sequence* sequence() const;
    void setTransport(Transport transport) { m_transport = std::move(transport); }

    [[nodiscard]] Tool tool() const noexcept { return m_tool; }
    void setTool(Tool tool);
    [[nodiscard]] bool snapping() const noexcept { return m_snapping; }
    void setSnapping(bool on);
    [[nodiscard]] const std::set<model::ClipId>& selection() const noexcept { return m_selection; }
    void setSelection(std::set<model::ClipId> clips);

    [[nodiscard]] Metrics& metrics() noexcept { return m_metrics; }
    void setPixelsPerSecond(double pps, std::optional<double> anchorX = std::nullopt);
    void zoomToFit();
    void scrollToTime(const Rational& t);
    [[nodiscard]] double contentWidthPixels() const;

    // Source patching targets used by insert/overwrite (null = that part is not placed).
    [[nodiscard]] commands::EditTargets targets() const;
    void setTargets(model::TrackId video, model::TrackId audio);

    // Repaints the playhead area if the transport position changed (called by a timer).
    void refreshPlayhead();

    // Edits (public for keyboard actions and tests).
    void deleteSelection();
    void rippleDeleteSelection();
    void razorAtPlayhead(bool allTracks);
    void toggleLinkSelection();
    // Clipboard (D18): copy / cut the selection, paste at the playhead on the target tracks.
    void copySelection();
    void cutSelection();
    void paste(bool insert);
    [[nodiscard]] const commands::Clipboard& clipboard() const noexcept { return m_clipboard; }
    void markIn();
    void markOut();
    void clearMarks();
    void goToIn();
    void goToOut();
    void lift();
    void extract();
    void goToEditPoint(bool next);
    void stepPlayhead(int frames);
    void togglePlay();
    void goToStart();
    void goToEnd();

signals:
    void statusMessage(const QString& text);
    void selectionChanged();
    void toolChanged(loki::ui::timeline::Tool tool);
    void snappingChanged(bool on);
    void zoomChanged(double pixelsPerSecond);
    void scrollChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Drag { None, Scrub, Move, TrimIn, TrimOut, Slip, Slide, Volume, FadeIn, FadeOut };

    // Audio clip overlays (Selection tool): volume line and fade handles.
    enum class AudioHandle { None, Level, FadeIn, FadeOut };
    struct AudioHit {
        AudioHandle handle = AudioHandle::None;
        model::ClipId clip;
    };
    [[nodiscard]] AudioHit audioHitAt(const QPoint& pos) const;
    [[nodiscard]] QRectF clipRect(const TrackRow& row, const model::Clip& clip) const;
    void paintAudioOverlay(QPainter& p, const QRectF& r, const model::Clip& clip) const;

    struct DropPreview {
        model::MediaId media;
        Rational at;
        Rational duration;
        std::optional<TrackRow> row;
    };

    // Content-area coordinates (x relative to the end of the headers).
    [[nodiscard]] double contentX(double widgetX) const noexcept { return widgetX - kHeaderWidth; }
    [[nodiscard]] std::vector<TrackRow> rows() const;
    [[nodiscard]] Rational playhead() const;
    [[nodiscard]] Rational frameRate() const;
    [[nodiscard]] Rational timeAt(double widgetX) const;
    [[nodiscard]] Rational snapped(const Rational& t, const std::set<model::ClipId>& exclude = {}) const;
    [[nodiscard]] const model::Track* trackOf(const TrackRow& row) const;

    void paintRuler(QPainter& p, const QRect& area) const;
    void paintHeaders(QPainter& p, const std::vector<TrackRow>& rows) const;
    void paintTrackClips(QPainter& p, const TrackRow& row, const model::Track& track) const;
    void paintClip(QPainter& p, const QRectF& r, const model::Clip& clip, model::TrackKind kind, bool selected) const;
    void paintWaveform(QPainter& p, const QRectF& r, const model::Clip& clip) const;
    void paintGhosts(QPainter& p, const std::vector<TrackRow>& rows) const;

    void headerClick(const TrackRow& row, const QPoint& pos);
    void selectWithLinked(model::ClipId clip, bool toggle, bool add);
    void runEdit(const std::function<std::unique_ptr<commands::Command>()>& make);
    void updateHoverCursor(const QPoint& pos);
    [[nodiscard]] std::optional<DropPreview> dropPreviewAt(const QMimeData* mime, const QPoint& pos) const;

    ProjectController& m_controller;
    MediaServices& m_services;
    model::SequenceId m_sequence;
    Transport m_transport;
    Metrics m_metrics;
    Tool m_tool = Tool::Selection;
    bool m_snapping = true;
    std::set<model::ClipId> m_selection;
    model::TrackId m_targetVideo;
    model::TrackId m_targetAudio;
    Rational m_lastPlayhead{-1};

    // Drag state.
    Drag m_drag = Drag::None;
    QPoint m_dragStart;
    model::ClipId m_dragClip;
    model::TrackKind m_dragKind = model::TrackKind::Video;
    std::size_t m_dragRowIndex = 0;
    Rational m_dragDelta;
    int m_dragTrackDelta = 0;
    bool m_dragLinked = true;
    commands::GestureId m_dragGesture = 0;
    Rational m_dragSourceTime; // volume drag: source time where the line was grabbed
    std::optional<Rational> m_snapIndicator;
    std::optional<DropPreview> m_dropPreview;
    commands::Clipboard m_clipboard;
};

} // namespace loki::ui::timeline

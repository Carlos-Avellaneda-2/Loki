#pragma once

#include "media/video_frame.h"
#include "media/waveform.h"
#include "render/composite.h"
#include "render/gl_compositor.h"
#include "render/gl_frame_renderer.h"

#include <QOpenGLWindow>
#include <QWidget>

#include <QElapsedTimer>

#include <functional>
#include <memory>

namespace loki::ui {

class VideoWindow;

// Monitor surface: draws the current video frame with OpenGL (letterboxed, black background).
// For audio-only media it draws the waveform instead.
//
// Implementation note: the GL surface is a native child window (QOpenGLWindow embedded with
// QWidget::createWindowContainer) instead of a QOpenGLWidget. A QOpenGLWidget switches the whole
// top-level window to OpenGL composition; with it the main window rendered blank in testing on an
// Intel HD 620 (root cause not confirmed). A native child window keeps the rest of the UI on the
// regular raster path, and video presentation does not depend on widget composition.
class VideoView final : public QWidget {
    Q_OBJECT

public:
    explicit VideoView(QWidget* parent = nullptr);
    ~VideoView() override;

    VideoView(const VideoView&) = delete;
    VideoView& operator=(const VideoView&) = delete;
    VideoView(VideoView&&) = delete;
    VideoView& operator=(VideoView&&) = delete;

    // Schedules a repaint only when the frame actually changed.
    void setFrame(media::VideoFramePtr frame);
    // Multitrack picture (sequences); null composite shows black.
    void setComposite(render::CompositePtr composite);
    void setWaveform(std::shared_ptr<const media::Waveform> waveform, Rational position);
    void clear();

    [[nodiscard]] const media::VideoFramePtr& frame() const noexcept;
    [[nodiscard]] bool rendererReady() const noexcept;
    [[nodiscard]] VideoWindow* window() const noexcept { return m_window; }
    // Scopes (D32): after compositing a new picture, and while `wanted()` is true, the picture is
    // read back and passed to `tap` (at most every `intervalMs`).
    void setFrameTap(std::function<bool()> wanted, std::function<void(const QImage&)> tap, int intervalMs = 100);

private:
    VideoWindow* m_window = nullptr; // owned by the container widget
};

// The OpenGL child window doing the actual drawing.
class VideoWindow final : public QOpenGLWindow {
    Q_OBJECT

public:
    VideoWindow();
    ~VideoWindow() override;

    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;
    VideoWindow(VideoWindow&&) = delete;
    VideoWindow& operator=(VideoWindow&&) = delete;

    void setFrame(media::VideoFramePtr frame);
    void setComposite(render::CompositePtr composite);
    void setWaveform(std::shared_ptr<const media::Waveform> waveform, Rational position);
    void clear();

    [[nodiscard]] const media::VideoFramePtr& frame() const noexcept { return m_frame; }
    [[nodiscard]] const render::CompositePtr& composite() const noexcept { return m_composite; }
    [[nodiscard]] bool rendererReady() const noexcept { return m_ready; }
    void setFrameTap(std::function<bool()> wanted, std::function<void(const QImage&)> tap, int intervalMs);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void releaseGl();
    void paintWaveform(); // QPainter on top of the GL clear (inside paintGL)

    render::GlFrameRenderer m_renderer;
    render::GlCompositor m_compositor;
    render::CompositePtr m_composite;
    render::CompositePtr m_rendered; // composite currently in the compositor's framebuffer
    int m_renderedWidth = 0;
    int m_renderedHeight = 0;
    media::VideoFramePtr m_frame;
    media::VideoFramePtr m_uploaded;
    std::shared_ptr<const media::Waveform> m_waveform;
    Rational m_waveformPosition;
    bool m_ready = false;
    std::function<bool()> m_tapWanted;
    std::function<void(const QImage&)> m_tap;
    int m_tapInterval = 100;
    QElapsedTimer m_tapTimer;
};

} // namespace loki::ui

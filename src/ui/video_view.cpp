#include "ui/video_view.h"

#include <QMouseEvent>

#include "render/color_conversion.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

namespace loki::ui {

// --- VideoView (container widget) --------------------------------------------------------------

VideoView::VideoView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto window = std::make_unique<VideoWindow>();
    m_window = window.get();
    // The container takes ownership of the window.
    QWidget* container = QWidget::createWindowContainer(window.release(), this);
    container->setFocusPolicy(Qt::NoFocus); // keyboard shortcuts stay with the panel
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
}

VideoView::~VideoView() = default;

void VideoView::setFrame(media::VideoFramePtr frame) {
    m_window->setFrame(std::move(frame));
}

void VideoView::setComposite(render::CompositePtr composite) {
    m_window->setComposite(std::move(composite));
}

void VideoView::setWaveform(std::shared_ptr<const media::Waveform> waveform, Rational position) {
    m_window->setWaveform(std::move(waveform), position);
}

void VideoView::clear() {
    m_window->clear();
}

const media::VideoFramePtr& VideoView::frame() const noexcept {
    return m_window->frame();
}

bool VideoView::rendererReady() const noexcept {
    return m_window->rendererReady();
}

// --- VideoWindow (OpenGL) ----------------------------------------------------------------------

VideoWindow::VideoWindow() : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate) {}

VideoWindow::~VideoWindow() {
    releaseGl();
}

void VideoWindow::releaseGl() {
    if (m_ready && context() != nullptr) {
        makeCurrent();
        m_renderer.release();
        m_compositor.release();
        doneCurrent();
    }
    m_ready = false;
}

void VideoView::setFrameTap(std::function<bool()> wanted, std::function<void(const QImage&)> tap, int intervalMs) {
    m_window->setFrameTap(std::move(wanted), std::move(tap), intervalMs);
}

void VideoView::setOverlay(VideoOverlay overlay) {
    m_window->setOverlay(std::move(overlay));
}

void VideoView::updateOverlay() {
    m_window->requestUpdate();
}

void VideoWindow::setOverlay(VideoOverlay overlay) {
    m_overlay = std::move(overlay);
    update();
}

QRectF VideoWindow::pictureRect() const {
    if (!m_composite || m_composite->width <= 0) {
        return {};
    }
    const auto r = render::fitRect(static_cast<float>(m_composite->width), static_cast<float>(m_composite->height),
                                   static_cast<float>(width()), static_cast<float>(height()));
    return {r[0], r[1], r[2], r[3]};
}

bool VideoWindow::mouseToOverlay(QMouseEvent* event) {
    const auto picture = pictureRect();
    if (!m_overlay.mouse || picture.isEmpty() || !m_composite) {
        return false;
    }
    const double scale = m_composite->width / picture.width();
    const QPointF canvas = (event->position() - picture.topLeft()) * scale;
    return m_overlay.mouse(static_cast<int>(event->type()), event->button(), event->modifiers(), canvas, scale);
}

void VideoWindow::mousePressEvent(QMouseEvent* event) {
    if (!mouseToOverlay(event)) {
        QOpenGLWindow::mousePressEvent(event);
    }
}

void VideoWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!mouseToOverlay(event)) {
        QOpenGLWindow::mouseMoveEvent(event);
    }
}

void VideoWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (!mouseToOverlay(event)) {
        QOpenGLWindow::mouseReleaseEvent(event);
    }
}

void VideoWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!mouseToOverlay(event)) {
        QOpenGLWindow::mouseDoubleClickEvent(event);
    }
}

void VideoWindow::setFrameTap(std::function<bool()> wanted, std::function<void(const QImage&)> tap, int intervalMs) {
    m_tapWanted = std::move(wanted);
    m_tap = std::move(tap);
    m_tapInterval = intervalMs;
    m_tapTimer.invalidate();
    m_rendered.reset(); // re-render (and tap) the current picture
    update();
}

void VideoWindow::setComposite(render::CompositePtr composite) {
    if (composite == m_composite && !m_frame && !m_waveform) {
        return;
    }
    m_composite = std::move(composite);
    m_frame.reset();
    m_waveform.reset();
    update();
}

void VideoWindow::setFrame(media::VideoFramePtr frame) {
    if (frame == m_frame && !m_waveform && !m_composite) {
        return;
    }
    m_frame = std::move(frame);
    m_waveform.reset();
    m_composite.reset();
    update();
}

void VideoWindow::setWaveform(std::shared_ptr<const media::Waveform> waveform, Rational position) {
    if (waveform == m_waveform && position == m_waveformPosition) {
        return;
    }
    m_waveform = std::move(waveform);
    m_waveformPosition = position;
    m_frame.reset();
    m_composite.reset();
    update();
}

void VideoWindow::clear() {
    m_frame.reset();
    m_waveform.reset();
    m_composite.reset();
    update();
}

void VideoWindow::initializeGL() {
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        m_renderer.release();
        m_compositor.release();
        doneCurrent();
        m_ready = false;
        m_uploaded.reset();
        m_rendered.reset();
    });
    m_ready = m_renderer.initialize() && m_compositor.initialize();
    static bool logged = false; // log the driver once per process
    if (!logged) {
        logged = true;
        auto* f = context()->functions();
        const auto str = [f](GLenum name) {
            const auto* s = reinterpret_cast<const char*>(f->glGetString(name));
            return s != nullptr ? std::string(s) : std::string("?");
        };
        const auto fmt = context()->format();
        spdlog::info("OpenGL {}.{} {} | {} | {} | renderer ready: {}", fmt.majorVersion(), fmt.minorVersion(),
                     fmt.profile() == QSurfaceFormat::CoreProfile ? "core" : "compat", str(GL_VENDOR), str(GL_RENDERER),
                     m_ready);
    }
}

void VideoWindow::paintGL() {
    const qreal dpr = devicePixelRatio();
    const int w = static_cast<int>(width() * dpr);
    const int h = static_cast<int>(height() * dpr);
    if (m_ready && m_composite && m_composite->width > 0) {
        // Composite at the smaller of the sequence size and the on-screen size (preview cost).
        const auto fit = render::fitRect(static_cast<float>(m_composite->width), static_cast<float>(m_composite->height),
                                         static_cast<float>(w), static_cast<float>(h));
        const int fw = std::clamp(static_cast<int>(fit[2]), 2, m_composite->width);
        const int fh = std::clamp(static_cast<int>(fit[3]), 2, m_composite->height);
        if (m_composite != m_rendered || fw != m_renderedWidth || fh != m_renderedHeight) {
            m_compositor.render(*m_composite, fw, fh);
            m_rendered = m_composite;
            m_renderedWidth = fw;
            m_renderedHeight = fh;
            if (m_tap && m_tapWanted && m_tapWanted() &&
                (!m_tapTimer.isValid() || m_tapTimer.elapsed() >= m_tapInterval)) {
                m_tapTimer.start();
                m_tap(m_compositor.readback());
            }
        }
        m_compositor.present(defaultFramebufferObject(), w, h);
        if (m_overlay.paint) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            m_overlay.paint(painter, pictureRect(), QSizeF(m_composite->width, m_composite->height));
        }
        return;
    }
    if (m_ready && m_frame) {
        if (m_frame != m_uploaded) {
            m_renderer.upload(*m_frame);
            m_uploaded = m_frame;
        }
        m_renderer.draw(w, h);
        return;
    }
    // No frame (or audio-only: the waveform is painted on top): plain black.
    auto* f = context()->functions();
    f->glViewport(0, 0, w, h);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    paintWaveform();
}

void VideoWindow::paintWaveform() {
    if (!m_waveform || m_waveform->peakCount() == 0) {
        return;
    }
    // Audio-only media: waveform overview with the playhead.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const int channels = m_waveform->channels;
    const qreal laneHeight = static_cast<qreal>(height()) / channels;
    const auto peaks = m_waveform->peakCount();
    const auto columns = static_cast<std::size_t>(std::max(1, width()));
    painter.setPen(QColor(0x2d, 0x8c, 0xeb));
    for (int c = 0; c < channels; ++c) {
        const qreal mid = laneHeight * (c + 0.5);
        for (std::size_t x = 0; x < columns; ++x) {
            const auto from = peaks * x / columns;
            const auto to = std::max(from + 1, peaks * (x + 1) / columns);
            int lo = 0;
            int hi = 0;
            for (auto p = from; p < std::min(to, peaks); ++p) {
                lo = std::min<int>(lo, m_waveform->min(p, c));
                hi = std::max<int>(hi, m_waveform->max(p, c));
            }
            const qreal scale = laneHeight / 2.0 / 32768.0 * 0.95;
            const auto px = static_cast<qreal>(x);
            painter.drawLine(QPointF(px, mid - hi * scale), QPointF(px, mid - lo * scale));
        }
    }
    const auto d = m_waveform->duration();
    if (d.isPositive()) {
        const qreal px = width() * (m_waveformPosition / d).toDouble();
        painter.setPen(QColor(0xe6, 0xe6, 0xe6));
        painter.drawLine(QPointF(px, 0), QPointF(px, height()));
    }
}

} // namespace loki::ui

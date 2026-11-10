#include "ui/scopes_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace loki::ui {

namespace {

constexpr int kLevels = render::ScopeData::kLevels;
const QColor kGraticule(0x60, 0x60, 0x60);
const QColor kLabel(0x90, 0x90, 0x90);

// Hit count -> trace brightness (0..1): a log curve so faint traces stay visible.
double intensity(std::uint32_t count, double scale) {
    return count == 0 ? 0.0 : std::min(1.0, std::log1p(static_cast<double>(count)) * scale);
}

// Trace image of a columns x kLevels grid in `color` (level 0 at the bottom).
QImage traceImage(const std::vector<std::uint32_t>& grid, int columns, const QColor& color, double scale) {
    QImage img(columns, kLevels, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int level = 0; level < kLevels; ++level) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(kLevels - 1 - level));
        for (int x = 0; x < columns; ++x) {
            const double v = intensity(grid[static_cast<std::size_t>(level) * static_cast<std::size_t>(columns) +
                                            static_cast<std::size_t>(x)],
                                       scale);
            if (v > 0.0) {
                const int a = static_cast<int>(v * 255.0);
                line[x] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), a));
            }
        }
    }
    return img;
}

// Brightness scale so a column full of one level is fully bright.
double scaleFor(std::uint64_t samples, int columns) {
    const double perColumn = static_cast<double>(samples) / std::max(1, columns);
    return 1.0 / std::max(1.0, std::log1p(perColumn) * 0.6);
}

void paintLevelGraticule(QPainter& p, const QRect& area) {
    QFont f = p.font();
    f.setPointSizeF(7.0);
    p.setFont(f);
    for (int ire = 0; ire <= 100; ire += 25) {
        const double y = area.bottom() - area.height() * ire / 100.0;
        p.setPen(QPen(kGraticule, 1, ire % 50 == 0 ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        p.setPen(kLabel);
        p.drawText(QPointF(area.left() + 2, y - 2), QString::number(ire));
    }
}

} // namespace

// --- ScopesPanel ---------------------------------------------------------------------------------

ScopesPanel::ScopesPanel(QWidget* parent) : QWidget(parent) {
    m_modeBox = new QComboBox;
    m_modeBox->addItems({tr("Waveform (Luma)"), tr("RGB Parade"), tr("Vectorscope YUV"), tr("Histogram")});
    m_view = new ScopeView;
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Scope:")));
    top->addWidget(m_modeBox);
    top->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(top);
    layout->addWidget(m_view, 1);
    connect(m_modeBox, &QComboBox::currentIndexChanged, this, [this](int i) { setMode(static_cast<Mode>(i)); });
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<const render::ScopeData>>::finished, this, [this] {
        m_data = m_watcher.result();
        m_view->setData(m_data, m_mode);
        emit analyzed();
        if (!m_pending.isNull()) {
            startAnalysis();
        }
    });
}

ScopesPanel::~ScopesPanel() {
    // Only a started analysis may be waited for (an empty QFuture never finishes).
    if (m_watcher.isRunning()) {
        m_watcher.waitForFinished();
    }
}

bool ScopesPanel::wantsImages() const {
    return isVisible();
}

void ScopesPanel::setMode(Mode mode) {
    m_mode = mode;
    if (m_modeBox->currentIndex() != static_cast<int>(mode)) {
        m_modeBox->setCurrentIndex(static_cast<int>(mode));
    }
    m_view->setData(m_data, m_mode);
}

void ScopesPanel::setImage(const QImage& image) {
    if (image.isNull()) {
        return;
    }
    m_pending = image;
    if (!m_watcher.isRunning()) {
        startAnalysis();
    }
}

void ScopesPanel::startAnalysis() {
    // Bound the work: scopes do not need more than ~480 px of width.
    QImage image = m_pending.width() > 480 ? m_pending.scaledToWidth(480, Qt::FastTransformation) : m_pending;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    m_pending = QImage();
    m_watcher.setFuture(QtConcurrent::run([image]() -> std::shared_ptr<const render::ScopeData> {
        return std::make_shared<const render::ScopeData>(
            render::analyzeScopes(image.constBits(), image.width(), image.height(), static_cast<int>(image.bytesPerLine()),
                                  std::min(image.width(), 360)));
    }));
}

// --- ScopeView -----------------------------------------------------------------------------------

ScopeView::ScopeView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void ScopeView::setData(std::shared_ptr<const render::ScopeData> scopeData, ScopesPanel::Mode mode) {
    m_data = std::move(scopeData);
    m_mode = mode;
    update();
}

void ScopeView::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x10, 0x10, 0x10));
    const QRect area = rect().adjusted(24, 8, -8, -8);
    if (!m_data || m_data->samples == 0 || area.width() < 10 || area.height() < 10) {
        p.setPen(kLabel);
        p.drawText(rect(), Qt::AlignCenter, tr("Play or scrub the sequence to see the scopes"));
        return;
    }
    switch (m_mode) {
    case ScopesPanel::Mode::Waveform: paintWaveform(p, area); break;
    case ScopesPanel::Mode::Parade: paintParade(p, area); break;
    case ScopesPanel::Mode::Vectorscope: paintVectorscope(p, area); break;
    case ScopesPanel::Mode::Histogram: paintHistogram(p, area); break;
    }
}

void ScopeView::paintWaveform(QPainter& p, const QRect& area) const {
    const auto& d = *m_data;
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(area, traceImage(d.luma, d.columns, QColor(0x90, 0xf0, 0x90), scaleFor(d.samples, d.columns)));
    paintLevelGraticule(p, area);
}

void ScopeView::paintParade(QPainter& p, const QRect& area) const {
    const auto& d = *m_data;
    const QColor colors[3] = {QColor(0xff, 0x50, 0x50), QColor(0x50, 0xff, 0x50), QColor(0x60, 0x80, 0xff)};
    const int third = area.width() / 3;
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int c = 0; c < 3; ++c) {
        const QRect r(area.left() + c * third, area.top(), third - 4, area.height());
        p.drawImage(r, traceImage(d.parade[static_cast<std::size_t>(c)], d.columns, colors[c],
                                  scaleFor(d.samples, d.columns)));
    }
    paintLevelGraticule(p, area);
}

void ScopeView::paintVectorscope(QPainter& p, const QRect& area) const {
    const auto& d = *m_data;
    const int side = std::min(area.width(), area.height());
    const QRect square(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
    QImage img(kLevels, kLevels, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const double scale = 1.0 / std::max(1.0, std::log1p(static_cast<double>(d.samples) / 400.0));
    for (int cr = 0; cr < kLevels; ++cr) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(kLevels - 1 - cr)); // Cr up
        for (int cb = 0; cb < kLevels; ++cb) {
            const double v = intensity(d.vectorscope[static_cast<std::size_t>(cr) * kLevels + static_cast<std::size_t>(cb)], scale);
            if (v > 0.0) {
                line[cb] = qPremultiply(qRgba(0xd0, 0xf0, 0xd0, static_cast<int>(v * 255.0)));
            }
        }
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(kGraticule, 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(square);
    p.drawLine(QPointF(square.center().x(), square.top()), QPointF(square.center().x(), square.bottom()));
    p.drawLine(QPointF(square.left(), square.center().y()), QPointF(square.right(), square.center().y()));
    // 75 % color bar targets (R, Mg, B, Cy, G, Yl) and the skin tone line.
    const struct {
        int r, g, b;
        const char* name;
    } targets[] = {{191, 0, 0, "R"}, {191, 0, 191, "Mg"}, {0, 0, 191, "B"}, {0, 191, 191, "Cy"}, {0, 191, 0, "G"}, {191, 191, 0, "Yl"}};
    const auto toPoint = [&](int cb, int cr) {
        return QPointF(square.left() + (cb + 0.5) * side / kLevels, square.bottom() - (cr + 0.5) * side / kLevels);
    };
    QFont f = p.font();
    f.setPointSizeF(7.0);
    p.setFont(f);
    for (const auto& t : targets) {
        const auto c = render::toYCbCr709(t.r, t.g, t.b);
        const QPointF pt = toPoint(c.cb, c.cr);
        p.setPen(kLabel);
        p.drawRect(QRectF(pt.x() - 4, pt.y() - 4, 8, 8));
        p.drawText(pt + QPointF(6, -4), QString::fromLatin1(t.name));
    }
    const double skin = 123.0 * 3.14159265358979 / 180.0; // the I line
    p.setPen(QPen(QColor(0xc0, 0x90, 0x60, 160), 1, Qt::DashLine));
    p.drawLine(QPointF(square.center()), QPointF(square.center().x() + std::cos(skin) * side / 2.0,
                                                 square.center().y() - std::sin(skin) * side / 2.0));
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(square, img);
}

void ScopeView::paintHistogram(QPainter& p, const QRect& area) const {
    const auto& d = *m_data;
    std::uint32_t peak = 1;
    for (std::size_t c = 0; c < 3; ++c) {
        for (const auto v : d.histogram[c]) {
            peak = std::max(peak, v);
        }
    }
    const QColor colors[3] = {QColor(255, 70, 70, 140), QColor(70, 255, 70, 140), QColor(90, 120, 255, 140)};
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (std::size_t c = 0; c < 3; ++c) {
        QPolygonF poly;
        poly << QPointF(area.left(), area.bottom());
        for (int i = 0; i < kLevels; ++i) {
            const double h = std::sqrt(static_cast<double>(d.histogram[c][static_cast<std::size_t>(i)]) / peak);
            poly << QPointF(area.left() + area.width() * (i + 0.5) / kLevels, area.bottom() - h * area.height());
        }
        poly << QPointF(area.right(), area.bottom());
        p.setPen(Qt::NoPen);
        p.setBrush(colors[c]);
        p.drawPolygon(poly);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(QPen(kGraticule, 1, Qt::DotLine));
    for (int q = 1; q < 4; ++q) {
        const double x = area.left() + area.width() * q / 4.0;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
}

} // namespace loki::ui

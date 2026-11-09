#include "ui/audio_meters.h"

#include "ui/audio_scale.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace loki::ui {

namespace {

constexpr double kMeterMaxDb = 0.0; // top of the meter: 0 dBFS
constexpr int kScaleWidth = 30;
constexpr int kClipLight = 6;

double toDb(float linear) {
    return std::max(LevelMeter::kFloorDb, audio::gainToDb(linear));
}

QColor colorFor(double db) {
    if (db >= -6.0) {
        return {0xe0, 0x40, 0x30};
    }
    if (db >= -18.0) {
        return {0xe0, 0xc0, 0x30};
    }
    return {0x30, 0xc0, 0x50};
}

} // namespace

LevelMeter::LevelMeter(QWidget* parent, bool showScale) : QWidget(parent), m_showScale(showScale) {
    setMinimumSize(showScale ? 20 + kScaleWidth : 14, 60);
    setToolTip(tr("Peak / RMS level. Click to reset the clip lights."));
}

QSize LevelMeter::sizeHint() const {
    return {m_showScale ? 30 + kScaleWidth : 18, 160};
}

void LevelMeter::apply(Side& side, double peakDb, double rmsDb, bool clip, double dt,
                       std::chrono::steady_clock::time_point now) {
    const double fall = kFallDbPerSecond * dt;
    side.peakDb = std::max(peakDb, std::max(kFloorDb, side.peakDb - fall));
    side.rmsDb = std::max(rmsDb, std::max(kFloorDb, side.rmsDb - fall));
    const double held = std::chrono::duration<double>(now - side.holdSince).count();
    if (peakDb >= side.holdDb || held > kHoldSeconds) {
        side.holdDb = peakDb >= side.holdDb ? peakDb : std::max(kFloorDb, side.peakDb);
        side.holdSince = now;
    }
    side.clipped = side.clipped || clip;
}

void LevelMeter::setLevel(const audio::StereoLevel& level, std::chrono::steady_clock::time_point now) {
    const double dt = m_hasLast ? std::chrono::duration<double>(now - m_last).count() : 0.0;
    m_last = now;
    m_hasLast = true;
    apply(m_sides[0], toDb(level.left.peak), toDb(level.left.rms), level.left.peak >= 1.0f, dt, now);
    apply(m_sides[1], toDb(level.right.peak), toDb(level.right.rms), level.right.peak >= 1.0f, dt, now);
    update();
}

void LevelMeter::setSilent(std::chrono::steady_clock::time_point now) {
    if (m_sides[0].peakDb <= kFloorDb && m_sides[1].peakDb <= kFloorDb && m_sides[0].holdDb <= kFloorDb &&
        m_sides[1].holdDb <= kFloorDb) {
        m_hasLast = false;
        return; // already at rest: no repaint
    }
    setLevel({}, now);
}

void LevelMeter::resetClip() {
    m_sides[0].clipped = false;
    m_sides[1].clipped = false;
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent* event) {
    resetClip();
    event->accept();
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));
    const int scaleW = m_showScale ? kScaleWidth : 0;
    const QRect bars = rect().adjusted(2, kClipLight + 3, -2 - scaleW, -2);
    const int gap = 2;
    const int barW = std::max(2, (bars.width() - gap) / 2);
    const auto yOf = [&](double db) {
        return bars.bottom() - static_cast<int>(dbToScale(db, kMeterMaxDb) * bars.height());
    };
    for (int s = 0; s < 2; ++s) {
        const auto& side = m_sides[s];
        const QRect bar(bars.left() + s * (barW + gap), bars.top(), barW, bars.height());
        p.fillRect(bar, QColor(0x10, 0x10, 0x10));
        // Peak (dim) and RMS (solid), colored by zone.
        for (const auto& [db, alpha] : {std::pair{side.peakDb, 110}, std::pair{side.rmsDb, 255}}) {
            if (db <= kFloorDb) {
                continue;
            }
            for (const double zoneTop : {-18.0, -6.0, kMeterMaxDb}) {
                const double zoneBottom = zoneTop == -18.0 ? kFloorDb : (zoneTop == -6.0 ? -18.0 : -6.0);
                if (db <= zoneBottom) {
                    break;
                }
                const int y0 = yOf(std::min(db, zoneTop));
                const int y1 = yOf(zoneBottom);
                QColor c = colorFor(zoneBottom + 0.01);
                c.setAlpha(alpha);
                p.fillRect(QRect(bar.left(), y0, bar.width(), y1 - y0), c);
            }
        }
        if (side.holdDb > kFloorDb) {
            p.fillRect(QRect(bar.left(), yOf(side.holdDb), bar.width(), 2), colorFor(side.holdDb).lighter(130));
        }
        p.fillRect(QRect(bar.left(), 2, bar.width(), kClipLight),
                   side.clipped ? QColor(0xff, 0x30, 0x20) : QColor(0x40, 0x20, 0x20));
    }
    if (m_showScale) {
        QFont f = p.font();
        f.setPointSizeF(7.0);
        p.setFont(f);
        p.setPen(QColor(0x9a, 0x9a, 0x9a));
        for (const double db : {0.0, -6.0, -12.0, -18.0, -24.0, -36.0, -48.0, -60.0}) {
            const int y = yOf(db);
            p.drawText(QRect(bars.right() + 8, y - 7, scaleW - 8, 14), Qt::AlignVCenter | Qt::AlignLeft,
                       QString::number(static_cast<int>(db)));
        }
    }
}

AudioMetersPanel::AudioMetersPanel(QWidget* parent) : QWidget(parent) {
    m_meter = new LevelMeter(this, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_meter, 1, Qt::AlignHCenter);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &AudioMetersPanel::poll);
    m_timer->start();
}

void AudioMetersPanel::addSource(LevelSource source) {
    m_sources.push_back(std::move(source));
}

void AudioMetersPanel::poll() {
    for (const auto& source : m_sources) {
        const auto levels = source();
        if (levels.active) {
            m_meter->setLevel(levels.master);
            return;
        }
    }
    m_meter->setSilent();
}

} // namespace loki::ui

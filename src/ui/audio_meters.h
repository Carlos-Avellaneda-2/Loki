#pragma once

#include "audio/mixer.h"
#include "playback/playback_engine.h"

#include <QWidget>

#include <chrono>
#include <functional>
#include <vector>

class QTimer;

namespace loki::ui {

// Stereo peak/RMS meter with ballistics: the bar falls at kFallDbPerSecond, the peak marker holds
// for kHoldSeconds, and the clip light (a sample reached 0 dBFS) stays on until clicked.
class LevelMeter final : public QWidget {
    Q_OBJECT

public:
    static constexpr double kFallDbPerSecond = 24.0;
    static constexpr double kHoldSeconds = 1.5;
    static constexpr double kFloorDb = -60.0;

    explicit LevelMeter(QWidget* parent = nullptr, bool showScale = false);

    // New measurement (linear levels). `now` is injectable for tests.
    void setLevel(const audio::StereoLevel& level,
                  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // No signal (playback stopped): bars fall to the floor.
    void setSilent(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void resetClip();

    // Displayed values in dB (public for tests).
    [[nodiscard]] double peakDb(int side) const noexcept { return m_sides[side].peakDb; }
    [[nodiscard]] double rmsDb(int side) const noexcept { return m_sides[side].rmsDb; }
    [[nodiscard]] double holdDb(int side) const noexcept { return m_sides[side].holdDb; }
    [[nodiscard]] bool clipped() const noexcept { return m_sides[0].clipped || m_sides[1].clipped; }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    struct Side {
        double peakDb = kFloorDb;
        double rmsDb = kFloorDb;
        double holdDb = kFloorDb;
        std::chrono::steady_clock::time_point holdSince;
        bool clipped = false;
    };
    void apply(Side& side, double peakDb, double rmsDb, bool clip, double dt,
               std::chrono::steady_clock::time_point now);

    Side m_sides[2];
    std::chrono::steady_clock::time_point m_last;
    bool m_hasLast = false;
    bool m_showScale;
};

// Audio Meters panel: master levels of whatever is playing (Program or Source monitor).
class AudioMetersPanel final : public QWidget {
    Q_OBJECT

public:
    using LevelSource = std::function<playback::MixLevels()>;

    explicit AudioMetersPanel(QWidget* parent = nullptr);
    // Sources are polled in order; the first active one is shown.
    void addSource(LevelSource source);
    void poll();
    [[nodiscard]] LevelMeter* meter() const noexcept { return m_meter; }

private:
    std::vector<LevelSource> m_sources;
    LevelMeter* m_meter = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace loki::ui

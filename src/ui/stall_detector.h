#pragma once

#include <QObject>

#include <chrono>

class QTimer;

namespace loki::ui {

// Measures how long the UI thread could not run its event loop (criterion: never > 50 ms). A
// short timer fires every `tickMs`; when it fires late by more than `thresholdMs`, the stall is
// logged (spdlog warning) and counted.
class StallDetector final : public QObject {
    Q_OBJECT

public:
    static constexpr int kDefaultThresholdMs = 50;

    explicit StallDetector(QObject* parent = nullptr, int tickMs = 10, int thresholdMs = kDefaultThresholdMs);

    [[nodiscard]] int stalls() const noexcept { return m_stalls; }
    [[nodiscard]] double longestMs() const noexcept { return m_longestMs; }
    void reset();

signals:
    void stalled(double milliseconds);

private:
    void tick();

    QTimer* m_timer = nullptr;
    int m_tickMs;
    int m_thresholdMs;
    std::chrono::steady_clock::time_point m_last;
    int m_stalls = 0;
    double m_longestMs = 0;
};

} // namespace loki::ui

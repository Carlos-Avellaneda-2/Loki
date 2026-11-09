#include "ui/stall_detector.h"

#include <QTimer>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace loki::ui {

StallDetector::StallDetector(QObject* parent, int tickMs, int thresholdMs)
    : QObject(parent), m_tickMs(tickMs), m_thresholdMs(thresholdMs), m_last(std::chrono::steady_clock::now()) {
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(tickMs);
    connect(m_timer, &QTimer::timeout, this, &StallDetector::tick);
    m_timer->start();
}

void StallDetector::reset() {
    m_stalls = 0;
    m_longestMs = 0;
    m_last = std::chrono::steady_clock::now();
}

void StallDetector::tick() {
    const auto now = std::chrono::steady_clock::now();
    const double blocked = std::chrono::duration<double, std::milli>(now - m_last).count() - m_tickMs;
    m_last = now;
    if (blocked > m_thresholdMs) {
        ++m_stalls;
        m_longestMs = std::max(m_longestMs, blocked);
        spdlog::warn("UI thread blocked for {:.0f} ms", blocked);
        emit stalled(blocked);
    }
}

} // namespace loki::ui

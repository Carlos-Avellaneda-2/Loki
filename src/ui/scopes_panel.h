#pragma once

#include "render/scopes.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

#include <memory>

class QComboBox;

namespace loki::ui {

class ScopeView;

// Lumetri Scopes (D32): waveform (luma), RGB parade, vectorscope and histogram of the Program
// Monitor picture. Pictures arrive through setImage() (already reduced by the monitor); the
// analysis runs on a worker thread and at most one runs at a time (newer pictures replace the
// pending one), so playback never waits for the scopes.
class ScopesPanel final : public QWidget {
    Q_OBJECT

public:
    enum class Mode { Waveform = 0, Parade, Vectorscope, Histogram };

    explicit ScopesPanel(QWidget* parent = nullptr);
    ~ScopesPanel() override;

    ScopesPanel(const ScopesPanel&) = delete;
    ScopesPanel& operator=(const ScopesPanel&) = delete;
    ScopesPanel(ScopesPanel&&) = delete;
    ScopesPanel& operator=(ScopesPanel&&) = delete;

    // True while the panel is shown (the monitor only reads pictures back then).
    [[nodiscard]] bool wantsImages() const;
    void setImage(const QImage& image);
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] const std::shared_ptr<const render::ScopeData>& data() const noexcept { return m_data; }

signals:
    void analyzed();

private:
    void startAnalysis();

    Mode m_mode = Mode::Waveform;
    QComboBox* m_modeBox = nullptr;
    ScopeView* m_view = nullptr;
    QImage m_pending;
    std::shared_ptr<const render::ScopeData> m_data;
    QFutureWatcher<std::shared_ptr<const render::ScopeData>> m_watcher;
};

// Draws one scope of a ScopeData.
class ScopeView final : public QWidget {
    Q_OBJECT

public:
    explicit ScopeView(QWidget* parent = nullptr);
    void setData(std::shared_ptr<const render::ScopeData> data, ScopesPanel::Mode mode);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintWaveform(QPainter& p, const QRect& area) const;
    void paintParade(QPainter& p, const QRect& area) const;
    void paintVectorscope(QPainter& p, const QRect& area) const;
    void paintHistogram(QPainter& p, const QRect& area) const;

    std::shared_ptr<const render::ScopeData> m_data;
    ScopesPanel::Mode m_mode = ScopesPanel::Mode::Waveform;
};

} // namespace loki::ui

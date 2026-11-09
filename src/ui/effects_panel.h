#pragma once

#include <QWidget>

#include <string>

class QTreeWidget;

namespace loki::ui {

// Effects panel: video effects (Color Correction, LUT) and video/audio transitions. Items are
// dragged onto timeline clips (effects) or clip edges (transitions); double-click applies the item
// to the timeline selection / the edit point at the playhead.
class EffectsPanel final : public QWidget {
    Q_OBJECT

public:
    static constexpr auto kMimeType = "application/x-loki-effect";

    explicit EffectsPanel(QWidget* parent = nullptr);
    [[nodiscard]] QTreeWidget* tree() const noexcept { return m_tree; }

signals:
    // `type` is an effect or transition type id ("loki.color", "loki.crossDissolve", ...).
    void applyRequested(const QString& type, bool isTransition);

private:
    QTreeWidget* m_tree = nullptr;
};

} // namespace loki::ui

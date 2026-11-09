#include "ui/effects_panel.h"

#include "model/effect_registry.h"
#include "model/transitions.h"

#include <QHeaderView>
#include <QMimeData>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace loki::ui {

namespace {

constexpr int kTypeRole = Qt::UserRole + 1;
constexpr int kTransitionRole = Qt::UserRole + 2;

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// Tree that drags the item's type id.
class EffectsTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(EffectsPanel::kMimeType)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.isEmpty() || items.front()->data(0, kTypeRole).toString().isEmpty()) {
            return nullptr;
        }
        auto* mime = new QMimeData(); // NOLINT(cppcoreguidelines-owning-memory): Qt takes ownership
        const QString payload = (items.front()->data(0, kTransitionRole).toBool() ? QStringLiteral("transition:")
                                                                                 : QStringLiteral("effect:")) +
                                items.front()->data(0, kTypeRole).toString();
        mime->setData(QString::fromLatin1(EffectsPanel::kMimeType), payload.toUtf8());
        return mime;
    }
};

} // namespace

EffectsPanel::EffectsPanel(QWidget* parent) : QWidget(parent) {
    m_tree = new EffectsTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    const auto folder = [this](const QString& name) {
        auto* item = new QTreeWidgetItem(m_tree, {name});
        item->setFlags(Qt::ItemIsEnabled);
        item->setExpanded(true);
        return item;
    };
    auto* video = folder(tr("Video Effects"));
    for (const auto& def : model::effectRegistry()) {
        if (!def.fixed && def.kind == model::TrackKind::Video) {
            auto* item = new QTreeWidgetItem(video, {qs(def.label)});
            item->setData(0, kTypeRole, qs(def.type));
            item->setData(0, kTransitionRole, false);
            item->setToolTip(0, tr("Drag onto a video clip, or double-click to apply to the selected clips"));
        }
    }
    auto* videoTransitions = folder(tr("Video Transitions"));
    auto* audioTransitions = folder(tr("Audio Transitions"));
    for (const auto& def : model::transitionRegistry()) {
        auto* item = new QTreeWidgetItem(def.kind == model::TrackKind::Video ? videoTransitions : audioTransitions,
                                         {qs(def.label)});
        item->setData(0, kTypeRole, qs(def.type));
        item->setData(0, kTransitionRole, true);
        item->setToolTip(0, tr("Drag onto a cut or clip edge, or double-click to apply at the playhead"));
    }
    m_tree->expandAll();
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto type = item->data(0, kTypeRole).toString();
        if (!type.isEmpty()) {
            emit applyRequested(type, item->data(0, kTransitionRole).toBool());
        }
    });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);
}

} // namespace loki::ui

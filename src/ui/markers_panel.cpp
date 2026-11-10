#include "ui/markers_panel.h"

#include "commands/marker_commands.h"
#include "ui/marker_dialog.h"
#include "ui/project_controller.h"
#include "util/timecode.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace loki::ui {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kTimeNumRole = Qt::UserRole + 2;
constexpr int kTimeDenRole = Qt::UserRole + 3;

QString tc(const Rational& t, const Rational& rate) {
    return QString::fromStdString(formatTimecode(timeToFrames(t, rate, Rounding::Nearest), rate, supportsDropFrame(rate)));
}

} // namespace

MarkersPanel::MarkersPanel(ProjectController& controller, QWidget* parent) : QWidget(parent), m_controller(controller) {
    m_list = new QTreeWidget;
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Name"), tr("In"), tr("Duration"), tr("Comments")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setStretchLastSection(true);
    auto* edit = new QPushButton(tr("Edit..."));
    auto* remove = new QPushButton(tr("Delete"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(edit);
    buttons->addWidget(remove);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);

    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        emit seekRequested(Rational(item->data(0, kTimeNumRole).toLongLong(), item->data(0, kTimeDenRole).toLongLong()));
    });
    connect(edit, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedMarker(); id && m_editor) {
            m_editor(id);
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedMarker()) {
            try {
                if (auto command = commands::makeRemoveSequenceMarker(m_controller.project(), m_sequence, id)) {
                    m_controller.execute(std::move(command));
                }
            } catch (const std::exception&) {
            }
        }
    });
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (e.kind == model::ChangeKind::SequenceChanged && e.id == m_sequence.uuid()) {
            rebuild();
        }
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] { rebuild(); });
}

void MarkersPanel::setSequence(model::SequenceId id) {
    m_sequence = id;
    rebuild();
}

int MarkersPanel::markerCount() const {
    return m_list->topLevelItemCount();
}

model::MarkerId MarkersPanel::selectedMarker() const {
    const auto* item = m_list->currentItem();
    if (item == nullptr) {
        return {};
    }
    return model::MarkerId::parse(item->data(0, kIdRole).toString().toStdString()).value_or(model::MarkerId());
}

void MarkersPanel::rebuild() {
    const auto selected = selectedMarker();
    m_list->clear();
    const auto* seq = m_controller.project().findSequence(m_sequence);
    if (seq == nullptr) {
        return;
    }
    const auto rate = seq->settings.frameRate;
    for (const auto& m : seq->markers) {
        auto* item = new QTreeWidgetItem(m_list);
        QPixmap swatch(10, 10);
        swatch.fill(markerQColor(m.color));
        item->setIcon(0, QIcon(swatch));
        item->setText(0, m.name.empty() ? tr("Marker") : QString::fromStdString(m.name));
        item->setText(1, tc(m.time, rate));
        item->setText(2, tc(m.duration, rate));
        item->setText(3, QString::fromStdString(m.comment).replace(QLatin1Char('\n'), QLatin1Char(' ')));
        item->setData(0, kIdRole, QString::fromStdString(m.id.toString()));
        item->setData(0, kTimeNumRole, static_cast<qlonglong>(m.time.num()));
        item->setData(0, kTimeDenRole, static_cast<qlonglong>(m.time.den()));
        if (m.id == selected) {
            m_list->setCurrentItem(item);
        }
    }
}

} // namespace loki::ui

#include "ui/captions_panel.h"

#include "commands/caption_commands.h"
#include "ui/project_controller.h"
#include "util/atomic_file.h"
#include "util/timecode.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <filesystem>

namespace loki::ui {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

QString tc(const Rational& t, const Rational& rate) {
    return QString::fromStdString(formatTimecode(timeToFrames(t, rate, Rounding::Nearest), rate, supportsDropFrame(rate)));
}

std::filesystem::path pathOf(const QString& file) {
    return std::filesystem::path(file.toStdWString());
}

} // namespace

CaptionsPanel::CaptionsPanel(ProjectController& controller, QWidget* parent) : QWidget(parent), m_controller(controller) {
    m_list = new QTreeWidget;
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({tr("In"), tr("Out"), tr("Text")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setStretchLastSection(true);
    m_text = new QPlainTextEdit;
    m_text->setPlaceholderText(tr("Caption text (Ctrl+Enter applies)"));
    m_text->setMaximumHeight(70);
    auto* apply = new QPushButton(tr("Apply Text"));
    auto* add = new QPushButton(tr("Add at Playhead"));
    auto* remove = new QPushButton(tr("Delete"));
    auto* importButton = new QPushButton(tr("Import SRT..."));
    auto* exportButton = new QPushButton(tr("Export SRT..."));
    auto* buttons = new QHBoxLayout;
    for (auto* b : {add, remove, importButton, exportButton}) {
        buttons->addWidget(b);
    }
    buttons->addStretch(1);

    m_font = new QFontComboBox;
    m_size = new QDoubleSpinBox;
    m_size->setRange(8, 400);
    m_size->setSuffix(tr(" px"));
    m_color = new QToolButton;
    m_color->setMinimumWidth(48);
    m_background = new QDoubleSpinBox;
    m_background->setRange(0, 100);
    m_background->setSuffix(QStringLiteral(" %"));
    m_position_y = new QDoubleSpinBox;
    m_position_y->setRange(0, 100);
    m_position_y->setSuffix(QStringLiteral(" %"));
    m_visible = new QCheckBox(tr("Show captions in the monitors"));
    auto* style = new QFormLayout;
    style->addRow(tr("Font:"), m_font);
    style->addRow(tr("Size:"), m_size);
    style->addRow(tr("Color:"), m_color);
    style->addRow(tr("Background:"), m_background);
    style->addRow(tr("Vertical position:"), m_position_y);
    style->addRow(QString(), m_visible);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_list, 1);
    auto* textRow = new QHBoxLayout;
    textRow->addWidget(m_text, 1);
    textRow->addWidget(apply);
    layout->addLayout(textRow);
    layout->addLayout(buttons);
    layout->addLayout(style);

    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        const auto* seq = sequence();
        if (item == nullptr || seq == nullptr) {
            return;
        }
        for (const auto& c : seq->captions) {
            if (QString::fromStdString(c.id.toString()) == item->data(0, kIdRole).toString()) {
                m_text->setPlainText(QString::fromStdString(c.text));
                emit seekRequested(c.start);
            }
        }
    });
    connect(apply, &QPushButton::clicked, this, [this] { applyText(m_text->toPlainText()); });
    connect(add, &QPushButton::clicked, this, [this] { addAtPlayhead(); });
    connect(remove, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(importButton, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, tr("Import Captions"), QString(), tr("SubRip (*.srt)"));
        if (!file.isEmpty()) {
            importSrt(file);
        }
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getSaveFileName(this, tr("Export Captions"), QString(), tr("SubRip (*.srt)"));
        if (!file.isEmpty()) {
            exportSrt(file.endsWith(QStringLiteral(".srt"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".srt"));
        }
    });
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this] { applyStyle(); });
    connect(m_size, &QDoubleSpinBox::valueChanged, this, [this] { applyStyle(); });
    connect(m_background, &QDoubleSpinBox::valueChanged, this, [this] { applyStyle(); });
    connect(m_position_y, &QDoubleSpinBox::valueChanged, this, [this] { applyStyle(); });
    connect(m_visible, &QCheckBox::toggled, this, [this] { applyStyle(); });
    connect(m_color, &QToolButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(QColor(m_color->property("lokiColor").toString()), this, tr("Caption Color"));
        if (chosen.isValid()) {
            m_color->setProperty("lokiColor", chosen.name());
            applyStyle();
        }
    });
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (e.kind == model::ChangeKind::SequenceChanged && e.id == m_sequence.uuid()) {
            rebuild();
        }
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] { rebuild(); });
}

const model::Sequence* CaptionsPanel::sequence() const {
    return m_controller.project().findSequence(m_sequence);
}

void CaptionsPanel::setSequence(model::SequenceId id) {
    m_sequence = id;
    rebuild();
}

int CaptionsPanel::captionCount() const {
    return m_list->topLevelItemCount();
}

model::CaptionId CaptionsPanel::selectedCaption() const {
    const auto* item = m_list->currentItem();
    if (item == nullptr) {
        return {};
    }
    return model::CaptionId::parse(item->data(0, kIdRole).toString().toStdString()).value_or(model::CaptionId());
}

void CaptionsPanel::selectCaption(model::CaptionId id) {
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        auto* item = m_list->topLevelItem(i);
        if (item->data(0, kIdRole).toString() == QString::fromStdString(id.toString())) {
            m_list->setCurrentItem(item);
        }
    }
}

void CaptionsPanel::rebuild() {
    const auto selected = selectedCaption();
    m_updating = true;
    m_list->clear();
    if (const auto* seq = sequence()) {
        for (const auto& c : seq->captions) {
            auto* item = new QTreeWidgetItem(m_list);
            item->setText(0, tc(c.start, seq->settings.frameRate));
            item->setText(1, tc(c.end(), seq->settings.frameRate));
            item->setText(2, QString::fromStdString(c.text).replace(QLatin1Char('\n'), QStringLiteral(" / ")));
            item->setData(0, kIdRole, QString::fromStdString(c.id.toString()));
            if (c.id == selected) {
                m_list->setCurrentItem(item);
            }
        }
    }
    m_updating = false;
    refreshStyle();
}

void CaptionsPanel::refreshStyle() {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const auto& s = seq->captionStyle;
    m_updating = true;
    m_font->setCurrentFont(QFont(QString::fromStdString(s.font)));
    m_size->setValue(s.size);
    m_color->setProperty("lokiColor", QString::fromStdString(s.color));
    m_color->setStyleSheet(QStringLiteral("background: %1").arg(QString::fromStdString(s.color)));
    m_background->setValue(s.backgroundOpacity);
    m_position_y->setValue(s.positionY);
    m_visible->setChecked(s.visible);
    m_updating = false;
}

void CaptionsPanel::applyStyle() {
    const auto* seq = sequence();
    if (m_updating || seq == nullptr) {
        return;
    }
    auto style = seq->captionStyle;
    style.font = m_font->currentFont().family().toStdString();
    style.size = m_size->value();
    style.color = m_color->property("lokiColor").toString().toStdString();
    style.backgroundOpacity = m_background->value();
    style.positionY = m_position_y->value();
    style.visible = m_visible->isChecked();
    try {
        if (auto command = commands::makeSetCaptionStyle(m_controller.project(), m_sequence, style)) {
            m_controller.execute(std::move(command));
        }
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void CaptionsPanel::addAtPlayhead() {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return;
    }
    const Rational at = quantizeToFrame(m_position ? m_position() : Rational(), seq->settings.frameRate, Rounding::Floor);
    model::CaptionId id;
    try {
        m_controller.execute(commands::makeAddCaption(m_controller.project(), m_sequence, at, Rational(3),
                                                      tr("New caption").toStdString(), &id));
        selectCaption(id);
        m_text->setFocus();
        m_text->selectAll();
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void CaptionsPanel::applyText(const QString& text) {
    const auto* seq = sequence();
    const auto id = selectedCaption();
    if (seq == nullptr || !id) {
        return;
    }
    for (auto c : seq->captions) {
        if (c.id == id) {
            c.text = text.toStdString();
            try {
                if (auto command = commands::makeEditCaption(m_controller.project(), m_sequence, c)) {
                    m_controller.execute(std::move(command));
                }
            } catch (const std::exception& e) {
                emit m_controller.commandFailed(QString::fromUtf8(e.what()));
            }
            return;
        }
    }
}

void CaptionsPanel::deleteSelected() {
    if (const auto id = selectedCaption()) {
        if (auto command = commands::makeRemoveCaption(m_controller.project(), m_sequence, id)) {
            m_controller.execute(std::move(command));
        }
    }
}

bool CaptionsPanel::importSrt(const QString& file) {
    try {
        auto captions = commands::parseSrt(util::readFile(pathOf(file)));
        m_controller.execute(commands::makeReplaceCaptions(m_controller.project(), m_sequence, std::move(captions)));
        return true;
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(tr("Could not import captions: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
}

bool CaptionsPanel::exportSrt(const QString& file) {
    const auto* seq = sequence();
    if (seq == nullptr) {
        return false;
    }
    try {
        util::writeFileAtomic(pathOf(file), commands::formatSrt(seq->captions));
        return true;
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(tr("Could not export captions: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
}

} // namespace loki::ui

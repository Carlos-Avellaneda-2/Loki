#include "ui/effect_controls_panel.h"

#include "commands/transition_commands.h"
#include "model/transitions.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QFontComboBox>
#include <QLineEdit>

#include "commands/effect_commands.h"
#include "ui/project_controller.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace loki::ui {

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

const QColor kAccent(0x2d, 0x8c, 0xeb);
constexpr auto kGestureIdle = std::chrono::milliseconds(800);

} // namespace

// ---------------------------------------------------------------------------------------------
// EffectControlsPanel

EffectControlsPanel::EffectControlsPanel(ProjectController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    m_title = new QLabel(tr("(no clip selected)"), this);
    m_title->setContentsMargins(6, 4, 6, 4);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget(scroll);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(4, 0, 4, 4);
    m_bodyLayout->addStretch();
    scroll->setWidget(m_body);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_title);
    layout->addWidget(scroll, 1);

    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (!m_clip) {
            return;
        }
        if (e.kind == model::ChangeKind::ClipRemoved && e.id == m_clip.uuid() &&
            m_controller.project().findClip(m_clip).clip == nullptr) {
            setClip({});
            return;
        }
        if (m_transition && e.kind == model::ChangeKind::TrackChanged) {
            if (m_controller.project().findTransition(m_transition).transition == nullptr) {
                setTransition({});
            } else {
                m_lastPlayhead = Rational(-1);
                refreshValues();
            }
            return;
        }
        if (e.kind == model::ChangeKind::ClipChanged || e.kind == model::ChangeKind::ClipAdded) {
            if (const auto* c = currentClip(); c != nullptr && effectTypesFor(*c) != m_shownTypes) {
                rebuild(); // an effect was applied or removed
                return;
            }
            m_lastPlayhead = Rational(-1); // force a value refresh
            refreshValues();
        }
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] { setClip({}); });
    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, &EffectControlsPanel::refreshValues);
    m_timer->start();
}

const model::Clip* EffectControlsPanel::currentClip() const {
    return m_clip ? m_controller.project().findClip(m_clip).clip : nullptr;
}

void EffectControlsPanel::setClip(model::ClipId clip) {
    if (clip == m_clip && !m_rows.empty() && !m_transition) {
        return;
    }
    m_transition = {};
    m_clip = clip;
    rebuild();
}

void EffectControlsPanel::setTransition(model::TransitionId transition) {
    m_transition = transition;
    m_clip = {};
    rebuild();
}

std::vector<std::string> EffectControlsPanel::effectTypesFor(const model::Clip& c) const {
    std::vector<std::string> out;
    const auto ref = m_controller.project().findClip(c.id);
    const auto* media = m_controller.project().findMedia(c.media);
    const std::string generator = media != nullptr ? media->generator : std::string();
    for (const auto& def : model::effectRegistry()) {
        if (ref.track != nullptr && model::fixedEffectApplies(def, ref.track->kind, generator)) {
            out.push_back(def.type);
        }
    }
    for (const auto& e : c.effects) {
        const auto* def = model::findEffectDef(e.type);
        if (def != nullptr && !def->fixed && std::find(out.begin(), out.end(), e.type) == out.end()) {
            out.push_back(e.type);
        }
    }
    return out;
}

void EffectControlsPanel::setString(const std::string& effectType, const std::string& key, const std::string& value) {
    try {
        run(commands::setEffectString(m_controller.project(), m_clip, effectType, key, value));
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void EffectControlsPanel::rebuildTransition() {
    const auto ref = m_controller.project().findTransition(m_transition);
    if (ref.transition == nullptr) {
        m_title->setText(tr("(no clip selected)"));
        return;
    }
    const auto* def = model::findTransitionDef(ref.transition->type);
    m_title->setText(def != nullptr ? QString::fromUtf8(def->label.data(), static_cast<qsizetype>(def->label.size()))
                                    : qs(ref.transition->type));
    auto* group = new QWidget(m_body);
    auto* grid = new QGridLayout(group);
    grid->setContentsMargins(0, 6, 0, 2);
    const Rational rate = ref.sequence->settings.frameRate;
    grid->addWidget(new QLabel(tr("Duration"), group), 0, 0);
    m_transitionDuration = new QDoubleSpinBox(group);
    m_transitionDuration->setRange(1, 100000);
    m_transitionDuration->setDecimals(0);
    m_transitionDuration->setSuffix(tr(" frames"));
    m_transitionDuration->setKeyboardTracking(false);
    grid->addWidget(m_transitionDuration, 0, 1);
    connect(m_transitionDuration, &QDoubleSpinBox::valueChanged, this, [this, rate](double frames) {
        if (m_updating) {
            return;
        }
        try {
            run(commands::setTransitionDuration(m_controller.project(), m_transition,
                                                framesToTime(static_cast<std::int64_t>(frames), rate),
                                                gestureFor(m_transitionDuration)));
        } catch (const std::exception& e) {
            emit m_controller.commandFailed(QString::fromUtf8(e.what()));
        }
    });
    grid->addWidget(new QLabel(tr("Alignment"), group), 1, 0);
    m_transitionAlignment = new QComboBox(group);
    m_transitionAlignment->addItems({tr("Center at Cut"), tr("Start at Cut"), tr("End at Cut")});
    grid->addWidget(m_transitionAlignment, 1, 1);
    connect(m_transitionAlignment, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_updating) {
            try {
                run(commands::setTransitionAlignment(m_controller.project(), m_transition,
                                                     static_cast<model::TransitionAlignment>(index)));
            } catch (const std::exception& e) {
                emit m_controller.commandFailed(QString::fromUtf8(e.what()));
            }
        }
    });
    auto* remove = new QToolButton(group);
    remove->setText(tr("Remove"));
    connect(remove, &QToolButton::clicked, this, [this] {
        run(std::make_unique<commands::RemoveTransitionCommand>(m_transition));
    });
    grid->addWidget(remove, 2, 1, Qt::AlignRight);
    grid->setColumnStretch(2, 1);
    m_bodyLayout->insertWidget(0, group);
    m_lastPlayhead = Rational(-1);
    refreshValues();
}

Rational EffectControlsPanel::playheadSourceTime() const {
    const auto* c = currentClip();
    if (c == nullptr) {
        return {};
    }
    const auto ref = m_controller.project().findClip(m_clip);
    const Rational rate = ref.sequence->settings.frameRate;
    Rational t = m_playhead.position ? m_playhead.position() : c->start;
    t = quantizeToFrame(t, rate, Rounding::Floor);
    // Keep inside the clip [start, end - 1 frame].
    t = min(max(t, c->start), max(c->start, c->end() - rate.reciprocal()));
    return c->toSourceTime(t);
}

commands::GestureId EffectControlsPanel::gestureFor(const void* editor) {
    const auto now = std::chrono::steady_clock::now();
    if (editor != m_gestureEditor || now - m_gestureTime > kGestureIdle) {
        m_gesture = m_controller.newGestureId();
        m_gestureEditor = editor;
    }
    m_gestureTime = now;
    return m_gesture;
}

void EffectControlsPanel::run(std::unique_ptr<commands::Command> command) {
    if (command) {
        m_controller.execute(std::move(command));
    }
}

void EffectControlsPanel::setValue(const std::string& effectType, const std::string& key, double value,
                                   commands::GestureId gesture) {
    try {
        run(commands::setEffectParameter(m_controller.project(), m_clip, effectType, key, value, playheadSourceTime(),
                                         gesture));
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void EffectControlsPanel::setAnimated(const std::string& effectType, const std::string& key, bool on) {
    try {
        run(commands::setParameterAnimated(m_controller.project(), m_clip, effectType, key, on, playheadSourceTime()));
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void EffectControlsPanel::toggleKeyframe(const std::string& effectType, const std::string& key) {
    try {
        run(commands::toggleKeyframe(m_controller.project(), m_clip, effectType, key, playheadSourceTime()));
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

void EffectControlsPanel::jumpKeyframe(const std::string& effectType, const std::string& key, bool forward) {
    const auto* c = currentClip();
    if (c == nullptr || !m_playhead.seek) {
        return;
    }
    const auto* effect = model::findEffectByType(*c, effectType);
    const auto* p = effect != nullptr ? effect->findParameter(key) : nullptr;
    if (p == nullptr) {
        return;
    }
    const Rational now = playheadSourceTime();
    std::optional<Rational> target;
    for (const auto& k : p->keyframes) {
        if (forward && k.time > now && (!target || k.time < *target)) {
            target = k.time;
        }
        if (!forward && k.time < now && (!target || k.time > *target)) {
            target = k.time;
        }
    }
    if (target) {
        m_playhead.seek(c->start + (*target - c->sourceIn));
    }
}

void EffectControlsPanel::resetEffect(const std::string& effectType) {
    try {
        run(commands::resetEffect(m_controller.project(), m_clip, effectType));
    } catch (const std::exception& e) {
        emit m_controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

std::vector<std::string> EffectControlsPanel::shownEffects() const {
    std::vector<std::string> out;
    for (const auto& row : m_rows) {
        if (out.empty() || out.back() != row.effectType) {
            out.push_back(row.effectType);
        }
    }
    return out;
}

void EffectControlsPanel::rebuild() {
    // Remove old rows (everything except the trailing stretch).
    while (m_bodyLayout->count() > 1) {
        auto* item = m_bodyLayout->takeAt(0);
        if (auto* w = item->widget()) {
            w->deleteLater();
        }
        delete item; // NOLINT(cppcoreguidelines-owning-memory): QLayout::takeAt transfers ownership
    }
    m_rows.clear();
    m_shownTypes.clear();
    m_transitionDuration = nullptr;
    m_transitionAlignment = nullptr;
    if (m_transition) {
        rebuildTransition();
        return;
    }
    const auto* c = currentClip();
    if (c == nullptr) {
        m_title->setText(tr("(no clip selected)"));
        return;
    }
    const auto* media = m_controller.project().findMedia(c->media);
    m_title->setText(qs(c->name.empty() && media != nullptr ? media->name : c->name));

    int insertAt = 0;
    m_shownTypes = effectTypesFor(*c);
    for (const auto& shownType : m_shownTypes) {
        const auto& def = *model::findEffectDef(shownType);
        auto* group = new QWidget(m_body);
        auto* grid = new QGridLayout(group);
        grid->setContentsMargins(0, 6, 0, 2);
        grid->setHorizontalSpacing(4);
        auto* header = new QLabel(QStringLiteral("<b>%1</b>").arg(qs(def.label)), group);
        auto* reset = new QToolButton(group);
        reset->setText(def.fixed ? tr("Reset") : tr("Remove"));
        reset->setAutoRaise(true);
        const std::string type = def.type;
        connect(reset, &QToolButton::clicked, this, [this, type] { resetEffect(type); });
        grid->addWidget(header, 0, 0, 1, 3);
        grid->addWidget(reset, 0, 3, 1, 3, Qt::AlignRight);
        int r = 1;
        for (const auto& pdef : def.parameters) {
            Row row;
            row.effectType = def.type;
            row.key = pdef.key;
            row.def = &pdef;
            const std::string key = pdef.key;
            if (pdef.animatable) {
                row.stopwatch = new QToolButton(group);
                row.stopwatch->setCheckable(true);
                row.stopwatch->setText(QStringLiteral("\xE2\x8F\xB1")); // stopwatch
                row.stopwatch->setToolTip(tr("Toggle animation"));
                row.stopwatch->setAutoRaise(true);
                connect(row.stopwatch, &QToolButton::clicked, this,
                        [this, type, key](bool on) { setAnimated(type, key, on); });
                grid->addWidget(row.stopwatch, r, 0);
            }
            grid->addWidget(new QLabel(qs(pdef.label), group), r, 1);
            if (!pdef.enumLabels.empty()) {
                row.combo = new QComboBox(group);
                for (const auto& label : pdef.enumLabels) {
                    row.combo->addItem(qs(label));
                }
                connect(row.combo, &QComboBox::currentIndexChanged, this, [this, type, key](int index) {
                    if (!m_updating) {
                        setValue(type, key, index, 0);
                    }
                });
                grid->addWidget(row.combo, r, 2);
            } else {
                row.spin = new QDoubleSpinBox(group);
                row.spin->setRange(pdef.minimum, pdef.maximum);
                row.spin->setDecimals(pdef.decimals);
                row.spin->setSingleStep(pdef.step);
                row.spin->setSuffix(pdef.unit.empty() ? QString() : QStringLiteral(" ") + qs(pdef.unit));
                row.spin->setKeyboardTracking(false);
                auto* spin = row.spin;
                connect(row.spin, &QDoubleSpinBox::valueChanged, this, [this, type, key, spin](double v) {
                    if (!m_updating) {
                        setValue(type, key, v, gestureFor(spin));
                    }
                });
                grid->addWidget(row.spin, r, 2);
            }
            if (pdef.animatable) {
                auto* prev = new QToolButton(group);
                prev->setText(QStringLiteral("\xE2\x97\x80"));
                prev->setToolTip(tr("Go to Previous Keyframe"));
                prev->setAutoRaise(true);
                connect(prev, &QToolButton::clicked, this, [this, type, key] { jumpKeyframe(type, key, false); });
                auto* add = new QToolButton(group);
                add->setText(QStringLiteral("\xE2\x97\x86"));
                add->setToolTip(tr("Add/Remove Keyframe"));
                add->setAutoRaise(true);
                connect(add, &QToolButton::clicked, this, [this, type, key] { toggleKeyframe(type, key); });
                auto* next = new QToolButton(group);
                next->setText(QStringLiteral("\xE2\x96\xB6"));
                next->setToolTip(tr("Go to Next Keyframe"));
                next->setAutoRaise(true);
                connect(next, &QToolButton::clicked, this, [this, type, key] { jumpKeyframe(type, key, true); });
                grid->addWidget(prev, r, 3);
                grid->addWidget(add, r, 4);
                grid->addWidget(next, r, 5);
                row.lane = new KeyframeLane(m_controller, *this, type, key, group);
                connect(row.lane, &KeyframeLane::seekRequested, this, [this](const Rational& sourceTime) {
                    const auto* clip = currentClip();
                    if (clip != nullptr && m_playhead.seek) {
                        m_playhead.seek(clip->start + (sourceTime - clip->sourceIn));
                    }
                });
                grid->addWidget(row.lane, r, 6);
            }
            m_rows.push_back(row);
            ++r;
        }
        for (const auto& sdef : def.strings) {
            Row row;
            row.effectType = def.type;
            row.key = sdef.key;
            row.stringDef = &sdef;
            const std::string key = sdef.key;
            grid->addWidget(new QLabel(qs(sdef.label), group), r, 1);
            switch (sdef.kind) {
            case model::StringKind::Font: {
                row.font = new QFontComboBox(group);
                connect(row.font, &QFontComboBox::currentFontChanged, this, [this, type, key](const QFont& f) {
                    if (!m_updating) {
                        setString(type, key, f.family().toStdString());
                    }
                });
                grid->addWidget(row.font, r, 2, 1, 5);
                break;
            }
            case model::StringKind::Color: {
                row.color = new QToolButton(group);
                row.color->setToolTip(tr("Choose color"));
                row.color->setMinimumWidth(48);
                auto* button = row.color;
                connect(row.color, &QToolButton::clicked, this, [this, type, key, button] {
                    const QColor current(button->property("lokiColor").toString());
                    const QColor chosen = QColorDialog::getColor(current, this, tr("Text Color"));
                    if (chosen.isValid()) {
                        setString(type, key, chosen.name().toStdString());
                    }
                });
                grid->addWidget(row.color, r, 2);
                break;
            }
            case model::StringKind::File: {
                row.line = new QLineEdit(group);
                row.line->setReadOnly(true);
                auto* browse = new QToolButton(group);
                browse->setText(QStringLiteral("..."));
                connect(browse, &QToolButton::clicked, this, [this, type, key] {
                    const QString file = QFileDialog::getOpenFileName(this, tr("Choose LUT"), QString(),
                                                                      tr("Cube LUT (*.cube)"));
                    if (!file.isEmpty()) {
                        setString(type, key, file.toStdString());
                    }
                });
                grid->addWidget(row.line, r, 2, 1, 4);
                grid->addWidget(browse, r, 6, Qt::AlignLeft);
                break;
            }
            case model::StringKind::Text: {
                row.line = new QLineEdit(group);
                auto* line = row.line;
                connect(row.line, &QLineEdit::editingFinished, this, [this, type, key, line] {
                    if (!m_updating) {
                        setString(type, key, line->text().toStdString());
                    }
                });
                grid->addWidget(row.line, r, 2, 1, 5);
                break;
            }
            }
            m_rows.push_back(row);
            ++r;
        }
        grid->setColumnStretch(6, 1);
        m_bodyLayout->insertWidget(insertAt++, group);
    }
    m_lastPlayhead = Rational(-1);
    refreshValues();
}

void EffectControlsPanel::refreshValues() {
    if (m_transition) {
        const auto ref = m_controller.project().findTransition(m_transition);
        if (ref.transition == nullptr || m_transitionDuration == nullptr || m_lastPlayhead == Rational(0)) {
            return;
        }
        m_lastPlayhead = Rational(0);
        m_updating = true;
        if (!m_transitionDuration->hasFocus()) {
            m_transitionDuration->setValue(static_cast<double>(
                timeToFrames(ref.transition->duration, ref.sequence->settings.frameRate, Rounding::Nearest)));
        }
        m_transitionAlignment->setCurrentIndex(static_cast<int>(ref.transition->alignment));
        m_updating = false;
        return;
    }
    const auto* c = currentClip();
    if (c == nullptr) {
        return;
    }
    const Rational now = playheadSourceTime();
    if (now == m_lastPlayhead) {
        return;
    }
    m_lastPlayhead = now;
    const auto ctx = commands::clipContext(m_controller.project(), m_clip);
    m_updating = true;
    for (auto& row : m_rows) {
        if (row.stringDef != nullptr) {
            const auto text = QString::fromStdString(model::effectiveString(*c, row.effectType, row.key));
            if (row.line != nullptr && !row.line->hasFocus() && row.line->text() != text) {
                row.line->setText(text);
            }
            if (row.font != nullptr && row.font->currentFont().family() != text) {
                row.font->setCurrentFont(QFont(text));
            }
            if (row.color != nullptr) {
                row.color->setProperty("lokiColor", text);
                row.color->setStyleSheet(QStringLiteral("background-color: %1;").arg(text));
            }
            continue;
        }
        const double value = model::effectiveValue(*c, row.effectType, row.key, now, ctx.media, ctx.settings);
        const auto* effect = model::findEffectByType(*c, row.effectType);
        const auto* p = effect != nullptr ? effect->findParameter(row.key) : nullptr;
        if (row.stopwatch != nullptr) {
            row.stopwatch->setChecked(p != nullptr && p->isAnimated());
        }
        if (row.spin != nullptr && !row.spin->hasFocus()) {
            row.spin->setValue(value);
        }
        if (row.combo != nullptr) {
            row.combo->setCurrentIndex(static_cast<int>(std::lround(value)));
        }
        if (row.lane != nullptr) {
            row.lane->setRange(m_clip, c->sourceIn, c->sourceOut());
            row.lane->setPlayheadSourceTime(now);
        }
    }
    m_updating = false;
}

// ---------------------------------------------------------------------------------------------
// KeyframeLane

KeyframeLane::KeyframeLane(ProjectController& controller, EffectControlsPanel& panel, std::string effectType,
                           std::string key, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_panel(panel), m_effectType(std::move(effectType)),
      m_key(std::move(key)) {
    setMinimumWidth(120);
    setFixedHeight(22);
}

void KeyframeLane::setRange(model::ClipId clip, const Rational& sourceIn, const Rational& sourceOut) {
    m_clip = clip;
    m_in = sourceIn;
    m_out = sourceOut;
    update();
}

void KeyframeLane::setPlayheadSourceTime(const Rational& t) {
    m_playhead = t;
    update();
}

const model::Parameter* KeyframeLane::parameter() const {
    const auto* clip = m_controller.project().findClip(m_clip).clip;
    const auto* effect = clip != nullptr ? model::findEffectByType(*clip, m_effectType) : nullptr;
    return effect != nullptr ? effect->findParameter(m_key) : nullptr;
}

double KeyframeLane::xOf(const Rational& t) const {
    const Rational span = m_out - m_in;
    if (!span.isPositive()) {
        return 6;
    }
    return 6 + (width() - 12) * ((t - m_in) / span).toDouble();
}

Rational KeyframeLane::timeAt(double x) const {
    const Rational span = m_out - m_in;
    const auto ref = m_controller.project().findClip(m_clip);
    if (!span.isPositive() || ref.sequence == nullptr) {
        return m_in;
    }
    const double f = std::clamp((x - 6) / std::max(1, width() - 12), 0.0, 1.0);
    const Rational rate = ref.sequence->settings.frameRate;
    const auto frames = static_cast<std::int64_t>(std::floor(f * span.toDouble() * rate.toDouble()));
    return m_in + framesToTime(frames, rate);
}

std::optional<Rational> KeyframeLane::keyframeAt(double x) const {
    const auto* p = parameter();
    if (p == nullptr) {
        return std::nullopt;
    }
    for (const auto& k : p->keyframes) {
        if (std::abs(xOf(k.time) - x) <= 5) {
            return k.time;
        }
    }
    return std::nullopt;
}

void KeyframeLane::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x1b, 0x1b, 0x1b));
    const double mid = height() / 2.0;
    if (const auto* param = parameter()) {
        for (const auto& k : param->keyframes) {
            Rational t = k.time;
            if (m_dragFrom && *m_dragFrom == k.time && m_dragTo) {
                t = *m_dragTo;
            }
            const double x = xOf(t);
            QPolygonF diamond;
            diamond << QPointF(x, mid - 5) << QPointF(x + 5, mid) << QPointF(x, mid + 5) << QPointF(x - 5, mid);
            p.setPen(Qt::NoPen);
            p.setBrush(k.interpolation == model::Interpolation::Hold     ? QColor(0xe0, 0xa0, 0x40)
                       : k.interpolation == model::Interpolation::Bezier ? QColor(0x80, 0xd0, 0xff)
                                                                        : QColor(0xdd, 0xdd, 0xdd));
            p.drawPolygon(diamond);
        }
    }
    const double px = xOf(m_playhead);
    p.setPen(QPen(kAccent, 1));
    p.drawLine(QPointF(px, 0), QPointF(px, height()));
}

void KeyframeLane::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const double x = event->position().x();
    m_dragFrom = keyframeAt(x);
    m_dragTo.reset();
    if (!m_dragFrom) {
        emit seekRequested(timeAt(x));
    }
}

void KeyframeLane::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragFrom) {
        m_dragTo = timeAt(event->position().x());
        update();
    }
}

void KeyframeLane::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragFrom && m_dragTo && *m_dragTo != *m_dragFrom) {
        try {
            if (auto command = commands::moveKeyframe(m_controller.project(), m_clip, m_effectType, m_key, *m_dragFrom,
                                                      *m_dragTo)) {
                m_controller.execute(std::move(command));
            }
        } catch (const std::exception& e) {
            emit m_controller.commandFailed(QString::fromUtf8(e.what()));
        }
    } else if (m_dragFrom) {
        emit seekRequested(*m_dragFrom);
    }
    m_dragFrom.reset();
    m_dragTo.reset();
    update();
}

void KeyframeLane::contextMenuEvent(QContextMenuEvent* event) {
    const auto time = keyframeAt(event->pos().x());
    if (!time) {
        return;
    }
    QMenu menu(this);
    const auto set = [this, t = *time](model::Interpolation i) {
        try {
            m_controller.execute(commands::setKeyframeInterpolation(m_controller.project(), m_clip, m_effectType, m_key, t, i));
        } catch (const std::exception& e) {
            emit m_controller.commandFailed(QString::fromUtf8(e.what()));
        }
    };
    menu.addAction(tr("Linear"), this, [set] { set(model::Interpolation::Linear); });
    menu.addAction(tr("Bezier (Ease)"), this, [set] { set(model::Interpolation::Bezier); });
    menu.addAction(tr("Hold"), this, [set] { set(model::Interpolation::Hold); });
    menu.addSeparator();
    menu.addAction(tr("Delete"), this, [this, t = *time] {
        try {
            m_controller.execute(commands::toggleKeyframe(m_controller.project(), m_clip, m_effectType, m_key, t));
        } catch (const std::exception& e) {
            emit m_controller.commandFailed(QString::fromUtf8(e.what()));
        }
    });
    menu.exec(event->globalPos());
}

} // namespace loki::ui

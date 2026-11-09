#include "ui/audio_mixer_panel.h"

#include "commands/project_commands.h"
#include "ui/audio_meters.h"
#include "ui/audio_scale.h"
#include "ui/project_controller.h"

#include <QDial>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace loki::ui {

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString dbText(double db) {
    return db <= audio::kSilenceDb ? QStringLiteral("-inf") : QString::number(db, 'f', 1);
}

} // namespace

int AudioMixerPanel::faderPosition(double db) {
    return static_cast<int>(std::lround(dbToScale(db, kFaderMaxDb) * kFaderSteps));
}

double AudioMixerPanel::faderDb(int position) {
    const double db = scaleToDb(static_cast<double>(position) / kFaderSteps, kFaderMaxDb);
    return db <= audio::kSilenceDb ? audio::kSilenceDb : std::round(db * 10.0) / 10.0;
}

AudioMixerPanel::AudioMixerPanel(ProjectController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* content = new QWidget(this);
    m_row = new QHBoxLayout(content);
    m_row->setContentsMargins(4, 4, 4, 4);
    m_row->setSpacing(4);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent&) {
        if (m_updating) {
            return;
        }
        rebuild(); // cheap no-op unless the track list changed; values refreshed either way
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] { rebuild(); });
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &AudioMixerPanel::poll);
    m_timer->start();
}

void AudioMixerPanel::setSequence(model::SequenceId id) {
    m_sequence = id;
    m_builtFor.clear();
    rebuild();
}

AudioMixerPanel::Strip AudioMixerPanel::makeStrip(model::TrackId track, const QString& name) {
    Strip s;
    s.track = track;
    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setMinimumWidth(78);
    auto* v = new QVBoxLayout(frame);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);
    s.name = new QLabel(name, frame);
    s.name->setAlignment(Qt::AlignCenter);
    v->addWidget(s.name);
    if (track) {
        s.pan = new QDial(frame);
        s.pan->setRange(-100, 100);
        s.pan->setNotchesVisible(true);
        s.pan->setFixedSize(40, 40);
        s.pan->setToolTip(tr("Pan / balance"));
        v->addWidget(s.pan, 0, Qt::AlignHCenter);
        auto* buttons = new QHBoxLayout();
        s.mute = new QToolButton(frame);
        s.mute->setText(QStringLiteral("M"));
        s.mute->setCheckable(true);
        s.mute->setToolTip(tr("Mute track"));
        s.solo = new QToolButton(frame);
        s.solo->setText(QStringLiteral("S"));
        s.solo->setCheckable(true);
        s.solo->setToolTip(tr("Solo track"));
        buttons->addWidget(s.mute);
        buttons->addWidget(s.solo);
        v->addLayout(buttons);
    } else {
        v->addSpacing(40 + 26);
    }
    auto* faderRow = new QHBoxLayout();
    s.fader = new QSlider(Qt::Vertical, frame);
    s.fader->setRange(0, kFaderSteps);
    s.fader->setMinimumHeight(120);
    s.fader->setToolTip(track ? tr("Track volume") : tr("Master volume"));
    s.meter = new LevelMeter(frame);
    faderRow->addWidget(s.fader);
    faderRow->addWidget(s.meter);
    v->addLayout(faderRow, 1);
    s.value = new QLabel(frame);
    s.value->setAlignment(Qt::AlignCenter);
    v->addWidget(s.value);
    m_row->addWidget(frame);
    return s;
}

void AudioMixerPanel::rebuild() {
    const auto* seq = m_controller.project().findSequence(m_sequence);
    std::vector<model::TrackId> ids;
    if (seq != nullptr) {
        for (const auto& t : seq->audioTracks) {
            ids.push_back(t.id);
        }
    }
    if (ids != m_builtFor || m_strips.empty() != (seq == nullptr)) {
        while (auto* item = m_row->takeAt(0)) {
            if (auto* w = item->widget()) {
                w->deleteLater();
            }
            delete item; // NOLINT(cppcoreguidelines-owning-memory): QLayout::takeAt transfers ownership
        }
        m_strips.clear();
        m_builtFor = ids;
        if (seq != nullptr) {
            for (const auto& t : seq->audioTracks) {
                m_strips.push_back(makeStrip(t.id, qs(t.state.name)));
            }
            m_strips.push_back(makeStrip({}, tr("Master")));
        }
        m_row->addStretch();
        for (std::size_t i = 0; i < m_strips.size(); ++i) {
            const auto& s = m_strips[i];
            connect(s.fader, &QSlider::sliderPressed, this, [this] { m_gesture = m_controller.newGestureId(); });
            connect(s.fader, &QSlider::sliderReleased, this, [this] { m_gesture = 0; });
            connect(s.fader, &QSlider::valueChanged, this, [this, i](int v) {
                if (!m_updating && i < m_strips.size()) {
                    onFader(m_strips[i], v);
                }
            });
            if (s.pan != nullptr) {
                connect(s.pan, &QDial::sliderPressed, this, [this] { m_gesture = m_controller.newGestureId(); });
                connect(s.pan, &QDial::sliderReleased, this, [this] { m_gesture = 0; });
                connect(s.pan, &QDial::valueChanged, this, [this, i](int v) {
                    if (!m_updating && i < m_strips.size()) {
                        onPan(m_strips[i], v);
                    }
                });
                const auto track = s.track;
                connect(s.mute, &QToolButton::clicked, this, [this, track](bool on) {
                    m_controller.execute(commands::setTrackMuted(m_controller.project(), track, on));
                });
                connect(s.solo, &QToolButton::clicked, this, [this, track](bool on) {
                    m_controller.execute(commands::setTrackSolo(m_controller.project(), track, on));
                });
            }
        }
    }
    refreshValues();
}

void AudioMixerPanel::refreshValues() {
    const auto* seq = m_controller.project().findSequence(m_sequence);
    if (seq == nullptr) {
        return;
    }
    m_updating = true;
    for (auto& s : m_strips) {
        double db = seq->masterVolumeDb;
        if (s.track) {
            const auto ref = m_controller.project().findTrack(s.track);
            if (ref.track == nullptr) {
                continue;
            }
            const auto& state = ref.track->state;
            db = state.volumeDb;
            s.name->setText(qs(state.name));
            s.pan->setValue(static_cast<int>(std::lround(state.pan)));
            s.pan->setToolTip(tr("Pan: %1").arg(state.pan, 0, 'f', 0));
            s.mute->setChecked(state.muted);
            s.solo->setChecked(state.solo);
        }
        s.fader->setValue(faderPosition(db));
        s.value->setText(dbText(db));
    }
    m_updating = false;
}

void AudioMixerPanel::onFader(const Strip& strip, int position) {
    const double db = faderDb(position);
    m_updating = true;
    const bool ok = strip.track
                        ? m_controller.execute(commands::setTrackVolume(m_controller.project(), strip.track, db, m_gesture))
                        : m_controller.execute(std::make_unique<commands::SetMasterVolumeCommand>(m_sequence, db, m_gesture));
    m_updating = false;
    if (ok) {
        strip.value->setText(dbText(db));
    }
}

void AudioMixerPanel::onPan(const Strip& strip, int value) {
    m_updating = true;
    m_controller.execute(commands::setTrackPan(m_controller.project(), strip.track, value, m_gesture));
    m_updating = false;
}

void AudioMixerPanel::poll() {
    const auto levels = m_levels ? m_levels() : playback::MixLevels{};
    for (auto& s : m_strips) {
        if (!levels.active) {
            s.meter->setSilent();
            continue;
        }
        if (!s.track) {
            s.meter->setLevel(levels.master);
            continue;
        }
        const auto it = std::find_if(levels.tracks.begin(), levels.tracks.end(),
                                     [&](const playback::TrackLevel& l) { return l.track == s.track; });
        if (it != levels.tracks.end()) {
            s.meter->setLevel(it->level);
        } else {
            s.meter->setSilent();
        }
    }
}

} // namespace loki::ui

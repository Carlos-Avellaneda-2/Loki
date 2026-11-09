#include "ui/program_monitor_panel.h"

#include "ui/project_controller.h"
#include "ui/scrub_bar.h"
#include "ui/sequence_edits.h"
#include "ui/shortcut_manager.h"
#include "ui/video_view.h"
#include "util/timecode.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace loki::ui {

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString timecode(const Rational& t, const Rational& rate) {
    return qs(formatTimecode(timeToFrames(t, rate, Rounding::Floor), rate, supportsDropFrame(rate)));
}

} // namespace

ProgramMonitorPanel::ProgramMonitorPanel(ProjectController& controller, playback::PlaybackOptions options,
                                         QWidget* parent)
    : QWidget(parent), m_controller(controller),
      m_engine(std::make_unique<playback::PlaybackEngine>(std::move(options))) {
    setFocusPolicy(Qt::StrongFocus);
    m_title = new QLabel(this);
    m_title->setContentsMargins(6, 2, 6, 2);
    m_view = new VideoView(this);
    m_scrub = new ScrubBar(this);
    m_timecode = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_timecode->setStyleSheet(QStringLiteral("color: #2d8ceb; font-family: Consolas, monospace; font-size: 11pt;"));
    m_durationLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_durationLabel->setStyleSheet(QStringLiteral("font-family: Consolas, monospace; font-size: 11pt;"));
    m_quality = new QComboBox(this);
    m_quality->addItems({tr("Full"), tr("1/2"), tr("1/4")});
    m_quality->setToolTip(tr("Playback Resolution"));

    const auto button = [this](QStyle::StandardPixmap icon, const QString& tip, auto slot) {
        auto* b = new QToolButton(this);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, slot);
        return b;
    };
    auto* start = button(QStyle::SP_MediaSkipBackward, tr("Go to Start (Home)"), [this] { m_engine->seek(Rational()); });
    auto* back = button(QStyle::SP_MediaSeekBackward, tr("Step Back (Left)"), [this] { m_engine->stepFrames(-1); });
    m_playButton = button(QStyle::SP_MediaPlay, tr("Play-Stop Toggle (Space)"), [this] { m_engine->togglePlay(); });
    auto* fwd = button(QStyle::SP_MediaSeekForward, tr("Step Forward (Right)"), [this] { m_engine->stepFrames(1); });
    auto* end = button(QStyle::SP_MediaSkipForward, tr("Go to End (End)"), [this] { m_engine->seek(m_engine->duration()); });

    auto* info = new QHBoxLayout();
    info->addWidget(m_timecode);
    info->addStretch();
    info->addWidget(m_quality);
    info->addStretch();
    info->addWidget(m_durationLabel);
    auto* transportRow = new QHBoxLayout();
    transportRow->addStretch();
    for (auto* w : {start, back, m_playButton, fwd, end}) {
        transportRow->addWidget(w);
    }
    transportRow->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(2);
    layout->addWidget(m_title);
    layout->addWidget(m_view, 1);
    layout->addLayout(info);
    layout->addWidget(m_scrub);
    layout->addLayout(transportRow);

    connect(m_scrub, &ScrubBar::scrubbed, this, [this](const Rational& t) { m_engine->seek(t); });
    connect(m_scrub, &ScrubBar::scrubStarted, this, [this] { m_engine->pause(); });
    connect(m_quality, &QComboBox::currentIndexChanged, this, &ProgramMonitorPanel::onQualityChanged);
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        switch (e.kind) {
        case model::ChangeKind::SequenceRemoved:
            if (e.id == m_sequence.uuid()) {
                setSequence({});
            }
            break;
        case model::ChangeKind::BinAdded:
        case model::ChangeKind::BinRemoved:
        case model::ChangeKind::BinChanged: break; // irrelevant for playback
        default: schedulePublish(); break;
        }
    });

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &ProgramMonitorPanel::tick);
    m_timer->start();
}

ProgramMonitorPanel::~ProgramMonitorPanel() = default;

void ProgramMonitorPanel::registerShortcuts(ShortcutManager& s) {
    const QString ctx = QStringLiteral("Program Monitor");
    const auto add = [&](const char* id, const QString& text, const QKeySequence& key, std::function<void()> fn) {
        s.create(this, QStringLiteral("program.") + QLatin1String(id), ctx, text, key, std::move(fn));
    };
    auto& e = *m_engine;
    add("play", tr("Play-Stop Toggle"), QKeySequence(Qt::Key_Space), [&e] { e.togglePlay(); });
    add("shuttleRight", tr("Shuttle Right"), QKeySequence(Qt::Key_L), [&e] { e.shuttle(playback::ShuttleKey::L); });
    add("shuttleStop", tr("Shuttle Stop"), QKeySequence(Qt::Key_K), [&e] { e.shuttle(playback::ShuttleKey::K); });
    add("shuttleLeft", tr("Shuttle Left"), QKeySequence(Qt::Key_J), [&e] { e.shuttle(playback::ShuttleKey::J); });
    add("stepBack", tr("Step Back 1 Frame"), QKeySequence(Qt::Key_Left), [&e] { e.stepFrames(-1); });
    add("stepForward", tr("Step Forward 1 Frame"), QKeySequence(Qt::Key_Right), [&e] { e.stepFrames(1); });
    add("stepBack5", tr("Step Back 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [&e] { e.stepFrames(-5); });
    add("stepForward5", tr("Step Forward 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [&e] { e.stepFrames(5); });
    add("start", tr("Go to Start"), QKeySequence(Qt::Key_Home), [&e] { e.seek(Rational()); });
    add("end", tr("Go to End"), QKeySequence(Qt::Key_End), [&e] { e.seek(e.duration()); });
    add("markIn", tr("Mark In"), QKeySequence(Qt::Key_I),
        [this] { sequence_edits::markIn(m_controller, m_sequence, m_engine->position()); });
    add("markOut", tr("Mark Out"), QKeySequence(Qt::Key_O),
        [this] { sequence_edits::markOut(m_controller, m_sequence, m_engine->position()); });
    add("goToIn", tr("Go to In"), QKeySequence(Qt::SHIFT | Qt::Key_I), [this] {
        if (const auto in = sequence_edits::markInOf(m_controller, m_sequence)) {
            m_engine->seek(*in);
        }
    });
    add("goToOut", tr("Go to Out"), QKeySequence(Qt::SHIFT | Qt::Key_O), [this] {
        if (const auto out = sequence_edits::markOutFrameOf(m_controller, m_sequence)) {
            m_engine->seek(*out);
        }
    });
    add("clearMarks", tr("Clear In and Out"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X),
        [this] { sequence_edits::clearMarks(m_controller, m_sequence); });
    add("lift", tr("Lift"), QKeySequence(Qt::Key_Semicolon), [this] { sequence_edits::lift(m_controller, m_sequence); });
    add("extract", tr("Extract"), QKeySequence(Qt::Key_Apostrophe),
        [this] { sequence_edits::extract(m_controller, m_sequence); });
}

void ProgramMonitorPanel::setSequence(model::SequenceId id) {
    m_sequence = id;
    if (!id) {
        m_engine->close();
        m_title->setText(tr("(no sequence)"));
        m_view->clear();
        return;
    }
    publishSnapshot();
}

timeline::Transport ProgramMonitorPanel::transport() {
    timeline::Transport t;
    t.position = [this] { return m_engine->position(); };
    t.seek = [this](const Rational& time) { m_engine->seek(time); };
    t.togglePlay = [this] { m_engine->togglePlay(); };
    t.step = [this](int frames) { m_engine->stepFrames(frames); };
    t.isPlaying = [this] { return m_engine->isPlaying(); };
    return t;
}

void ProgramMonitorPanel::schedulePublish() {
    if (m_publishScheduled || !m_sequence) {
        return;
    }
    m_publishScheduled = true;
    // Coalesce all change events of one command (macro) into one snapshot.
    QTimer::singleShot(0, this, [this] {
        m_publishScheduled = false;
        publishSnapshot();
    });
}

void ProgramMonitorPanel::publishSnapshot() {
    const auto& project = m_controller.project();
    const auto* seq = project.findSequence(m_sequence);
    if (seq == nullptr) {
        return;
    }
    auto snapshot = playback::makeSnapshot(project, m_sequence, ++m_generation);
    m_title->setText(qs(seq->name));
    m_scrub->setTiming(seq->duration(), seq->settings.frameRate);
    m_scrub->setMarks(seq->markIn, seq->markOut);
    m_durationLabel->setText(timecode(seq->duration(), seq->settings.frameRate));
    m_engine->openSequence(std::move(snapshot));
}

void ProgramMonitorPanel::onQualityChanged(int index) {
    const auto* seq = m_controller.project().findSequence(m_sequence);
    const int divisor = index == 2 ? 4 : index == 1 ? 2 : 1;
    if (divisor == 1 || seq == nullptr) {
        m_engine->setQuality(0, 0);
        return;
    }
    m_engine->setQuality(std::max(2, seq->settings.width / divisor), std::max(2, seq->settings.height / divisor));
}

QString ProgramMonitorPanel::timecodeText() const {
    return m_timecode->text();
}

void ProgramMonitorPanel::tick() {
    if (!m_sequence) {
        return;
    }
    const Rational position = m_engine->position();
    m_scrub->setPosition(position);
    m_timecode->setText(timecode(position, m_engine->frameRate()));
    m_playButton->setIcon(style()->standardIcon(m_engine->isPlaying() ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    (void)m_engine->currentFrame(); // advances the displayed picture with the clock
    const auto picture = m_engine->currentPicture();
    if (picture && picture->composite) {
        m_view->setComposite(picture->composite);
    } else {
        m_view->setComposite(nullptr);
    }
}

} // namespace loki::ui

#include "ui/source_monitor_panel.h"

#include "commands/project_commands.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/project_item_model.h"
#include "ui/scrub_bar.h"
#include "ui/shortcut_manager.h"
#include "ui/video_view.h"
#include "util/timecode.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
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
    const auto frames = timeToFrames(t, rate, Rounding::Floor);
    return qs(formatTimecode(frames, rate, supportsDropFrame(rate)));
}

} // namespace

SourceMonitorPanel::SourceMonitorPanel(ProjectController& controller, MediaServices& services,
                                       playback::PlaybackOptions options, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_services(services),
      m_engine(std::make_unique<playback::PlaybackEngine>(std::move(options))) {
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);

    m_title = new QLabel(tr("(no clip)"), this);
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
    auto* markInButton = new QToolButton(this);
    markInButton->setText(QStringLiteral("{"));
    markInButton->setToolTip(tr("Mark In (I)"));
    connect(markInButton, &QToolButton::clicked, this, &SourceMonitorPanel::markIn);
    auto* markOutButton = new QToolButton(this);
    markOutButton->setText(QStringLiteral("}"));
    markOutButton->setToolTip(tr("Mark Out (O)"));
    connect(markOutButton, &QToolButton::clicked, this, &SourceMonitorPanel::markOut);
    auto* goIn = button(QStyle::SP_MediaSkipBackward, tr("Go to In (Shift+I)"), &SourceMonitorPanel::goToIn);
    auto* back = button(QStyle::SP_MediaSeekBackward, tr("Step Back (Left)"), [this] { step(-1); });
    m_playButton = button(QStyle::SP_MediaPlay, tr("Play-Stop Toggle (Space)"), &SourceMonitorPanel::togglePlay);
    auto* fwd = button(QStyle::SP_MediaSeekForward, tr("Step Forward (Right)"), [this] { step(1); });
    auto* goOut = button(QStyle::SP_MediaSkipForward, tr("Go to Out (Shift+O)"), &SourceMonitorPanel::goToOut);
    auto* insertButton = new QToolButton(this);
    insertButton->setText(tr("Insert"));
    insertButton->setToolTip(tr("Insert (,)"));
    connect(insertButton, &QToolButton::clicked, this, [this] {
        if (m_media) {
            emit editRequested(m_media, true);
        }
    });
    auto* overwriteButton = new QToolButton(this);
    overwriteButton->setText(tr("Overwrite"));
    overwriteButton->setToolTip(tr("Overwrite (.)"));
    connect(overwriteButton, &QToolButton::clicked, this, [this] {
        if (m_media) {
            emit editRequested(m_media, false);
        }
    });

    auto* info = new QHBoxLayout();
    info->addWidget(m_timecode);
    info->addStretch();
    info->addWidget(m_quality);
    info->addStretch();
    info->addWidget(m_durationLabel);

    auto* transport = new QHBoxLayout();
    transport->addStretch();
    for (auto* w : {static_cast<QWidget*>(markInButton), static_cast<QWidget*>(markOutButton), static_cast<QWidget*>(goIn),
                    static_cast<QWidget*>(back), static_cast<QWidget*>(m_playButton), static_cast<QWidget*>(fwd),
                    static_cast<QWidget*>(goOut)}) {
        transport->addWidget(w);
    }
    transport->addSpacing(16);
    transport->addWidget(insertButton);
    transport->addWidget(overwriteButton);
    transport->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(2);
    layout->addWidget(m_title);
    layout->addWidget(m_view, 1);
    layout->addLayout(info);
    layout->addWidget(m_scrub);
    layout->addLayout(transport);

    connect(m_scrub, &ScrubBar::scrubbed, this, [this](const Rational& t) { m_engine->seek(t); });
    connect(m_scrub, &ScrubBar::scrubStarted, this, [this] { m_engine->pause(); });
    connect(m_quality, &QComboBox::currentIndexChanged, this, &SourceMonitorPanel::onQualityChanged);
    connect(&m_controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (e.id != m_media.uuid()) {
            return;
        }
        if (e.kind == model::ChangeKind::MediaRemoved) {
            unloadMedia();
        } else if (e.kind == model::ChangeKind::MediaChanged) {
            refreshMarks();
            if (const auto* item = currentItem()) {
                m_title->setText(qs(item->name));
            }
        }
    });
    connect(&m_controller, &ProjectController::projectReset, this, &SourceMonitorPanel::unloadMedia);
    connect(&m_services, &MediaServices::waveformReady, this, [this](const QString& id) {
        if (id.toStdString() == m_media.toString()) {
            tick();
        }
    });

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(10); // ~100 Hz polling; the view repaints only when the frame changes
    connect(m_timer, &QTimer::timeout, this, &SourceMonitorPanel::tick);
    m_timer->start();
}

SourceMonitorPanel::~SourceMonitorPanel() = default;

void SourceMonitorPanel::registerShortcuts(ShortcutManager& s) {
    const QString ctx = QStringLiteral("Source Monitor");
    const auto add = [&](const char* id, const QString& text, const QKeySequence& key, std::function<void()> fn) {
        s.create(this, QStringLiteral("source.") + QLatin1String(id), ctx, text, key, std::move(fn));
    };
    add("play", tr("Play-Stop Toggle"), QKeySequence(Qt::Key_Space), [this] { togglePlay(); });
    add("shuttleRight", tr("Shuttle Right"), QKeySequence(Qt::Key_L), [this] { shuttle(playback::ShuttleKey::L); });
    add("shuttleStop", tr("Shuttle Stop"), QKeySequence(Qt::Key_K), [this] { shuttle(playback::ShuttleKey::K); });
    add("shuttleLeft", tr("Shuttle Left"), QKeySequence(Qt::Key_J), [this] { shuttle(playback::ShuttleKey::J); });
    add("stepBack", tr("Step Back 1 Frame"), QKeySequence(Qt::Key_Left), [this] { step(-1); });
    add("stepForward", tr("Step Forward 1 Frame"), QKeySequence(Qt::Key_Right), [this] { step(1); });
    add("stepBack5", tr("Step Back 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { step(-5); });
    add("stepForward5", tr("Step Forward 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { step(5); });
    add("markIn", tr("Mark In"), QKeySequence(Qt::Key_I), [this] { markIn(); });
    add("markOut", tr("Mark Out"), QKeySequence(Qt::Key_O), [this] { markOut(); });
    add("goToIn", tr("Go to In"), QKeySequence(Qt::SHIFT | Qt::Key_I), [this] { goToIn(); });
    add("goToOut", tr("Go to Out"), QKeySequence(Qt::SHIFT | Qt::Key_O), [this] { goToOut(); });
    add("clearMarks", tr("Clear In and Out"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), [this] { clearInOut(); });
    add("start", tr("Go to Start"), QKeySequence(Qt::Key_Home), [this] { m_engine->seek(Rational()); });
    add("end", tr("Go to End"), QKeySequence(Qt::Key_End), [this] { m_engine->seek(m_engine->duration()); });
    add("insert", tr("Insert"), QKeySequence(Qt::Key_Comma), [this] {
        if (m_media) {
            emit editRequested(m_media, true);
        }
    });
    add("overwrite", tr("Overwrite"), QKeySequence(Qt::Key_Period), [this] {
        if (m_media) {
            emit editRequested(m_media, false);
        }
    });
}

const model::MediaItem* SourceMonitorPanel::currentItem() const {
    return m_media ? m_controller.project().findMedia(m_media) : nullptr;
}

void SourceMonitorPanel::loadMedia(model::MediaId id) {
    const auto* item = m_controller.project().findMedia(id);
    if (item == nullptr) {
        return;
    }
    if (item->isGenerated()) {
        emit m_controller.commandFailed(tr("Titles are edited in the timeline and Effect Controls"));
        return;
    }
    m_media = id;
    m_title->setText(qs(item->name));
    m_engine->open(item->path, item->info);
    m_scrub->setTiming(item->info.duration, item->info.frameRate.isPositive() ? item->info.frameRate : Rational(30));
    refreshMarks();
    if (item->markIn) {
        m_engine->seek(*item->markIn);
    }
    m_view->clear();
    setFocus();
}

void SourceMonitorPanel::unloadMedia() {
    m_media = {};
    m_engine->close();
    m_title->setText(tr("(no clip)"));
    m_scrub->setTiming({}, Rational(30));
    m_scrub->setMarks(std::nullopt, std::nullopt);
    m_view->clear();
}

void SourceMonitorPanel::togglePlay() {
    if (m_media) {
        m_engine->togglePlay();
    }
}

void SourceMonitorPanel::shuttle(playback::ShuttleKey key) {
    if (m_media) {
        m_engine->shuttle(key);
    }
}

void SourceMonitorPanel::step(int frames) {
    if (m_media) {
        m_engine->stepFrames(frames);
    }
}

Rational SourceMonitorPanel::currentFrameTime() const {
    return quantizeToFrame(m_engine->position(), m_engine->frameRate(), Rounding::Floor);
}

void SourceMonitorPanel::markIn() {
    if (m_media) {
        m_controller.execute(commands::setMediaMarkIn(m_media, currentFrameTime()));
    }
}

void SourceMonitorPanel::markOut() {
    if (m_media) {
        // Out is exclusive: mark the end of the current frame.
        m_controller.execute(commands::setMediaMarkOut(m_media, currentFrameTime() + m_engine->frameRate().reciprocal()));
    }
}

void SourceMonitorPanel::goToIn() {
    if (const auto* item = currentItem(); item != nullptr) {
        m_engine->pause();
        m_engine->seek(item->markIn.value_or(Rational()));
    }
}

void SourceMonitorPanel::goToOut() {
    if (const auto* item = currentItem(); item != nullptr) {
        m_engine->pause();
        const Rational out = item->markOut ? *item->markOut - m_engine->frameRate().reciprocal() : m_engine->duration();
        m_engine->seek(out);
    }
}

void SourceMonitorPanel::clearInOut() {
    if (const auto* item = currentItem(); item != nullptr && (item->markIn || item->markOut)) {
        std::vector<std::unique_ptr<commands::Command>> clear;
        clear.push_back(commands::setMediaMarkIn(m_media, std::nullopt));
        clear.push_back(commands::setMediaMarkOut(m_media, std::nullopt));
        m_controller.execute(std::make_unique<commands::MacroCommand>("Clear In and Out", std::move(clear)));
    }
}

void SourceMonitorPanel::refreshMarks() {
    const auto* item = currentItem();
    m_scrub->setMarks(item ? item->markIn : std::nullopt, item ? item->markOut : std::nullopt);
    if (item == nullptr) {
        return;
    }
    const Rational rate = m_engine->frameRate();
    const Rational in = item->markIn.value_or(Rational());
    const Rational out = item->markOut.value_or(item->info.duration);
    m_durationLabel->setText(timecode(max(Rational(), out - in), rate));
}

void SourceMonitorPanel::onQualityChanged(int index) {
    const auto* item = currentItem();
    const int divisor = index == 2 ? 4 : index == 1 ? 2 : 1;
    if (divisor == 1 || item == nullptr) {
        m_engine->setQuality(0, 0);
        return;
    }
    m_engine->setQuality(std::max(2, item->info.width / divisor), std::max(2, item->info.height / divisor));
}

QString SourceMonitorPanel::timecodeText() const {
    return m_timecode->text();
}

void SourceMonitorPanel::tick() {
    if (!m_media) {
        return;
    }
    if (m_engine->openCount() != m_seenOpenCount) {
        m_seenOpenCount = m_engine->openCount();
        // Timing from the decoder can be more precise than the probe.
        m_scrub->setTiming(m_engine->duration(), m_engine->frameRate());
        refreshMarks();
        if (!m_engine->isOpen()) {
            m_title->setText(m_title->text() + tr(" - cannot play: %1").arg(qs(m_engine->lastError())));
        }
    }
    const Rational position = m_engine->position();
    const Rational rate = m_engine->frameRate();
    m_scrub->setPosition(position);
    m_timecode->setText(timecode(position, rate));
    m_playButton->setIcon(style()->standardIcon(m_engine->isPlaying() ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));

    const auto* item = currentItem();
    if (item != nullptr && !item->info.hasVideo) {
        m_view->setWaveform(m_services.waveform(*item), position);
    } else {
        m_view->setFrame(m_engine->currentFrame());
    }
}

void SourceMonitorPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (!ProjectItemModel::decodeMediaIds(event->mimeData()).empty()) {
        event->acceptProposedAction();
    }
}

void SourceMonitorPanel::dropEvent(QDropEvent* event) {
    const auto ids = ProjectItemModel::decodeMediaIds(event->mimeData());
    if (!ids.empty()) {
        loadMedia(ids.front());
        event->acceptProposedAction();
    }
}

} // namespace loki::ui

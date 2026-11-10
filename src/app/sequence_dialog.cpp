#include "app/sequence_dialog.h"

#include <QComboBox>

#include <algorithm>
#include <vector>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace loki::app {

namespace {

QString rateLabel(const Rational& r) {
    const double fps = r.toDouble();
    const bool whole = r.den() == 1;
    return QString::number(fps, 'f', whole ? 0 : 3) + QStringLiteral(" fps");
}

} // namespace

const std::vector<SequenceDialog::Preset>& SequenceDialog::presets() {
    static const std::vector<Preset> kPresets = {
        {"HD 1080p 23.976", 1920, 1080, Rational(24000, 1001)},
        {"HD 1080p 24", 1920, 1080, Rational(24)},
        {"HD 1080p 25", 1920, 1080, Rational(25)},
        {"HD 1080p 29.97", 1920, 1080, Rational(30000, 1001)},
        {"HD 1080p 30", 1920, 1080, Rational(30)},
        {"HD 1080p 50", 1920, 1080, Rational(50)},
        {"HD 1080p 59.94", 1920, 1080, Rational(60000, 1001)},
        {"HD 1080p 60", 1920, 1080, Rational(60)},
        {"QHD 1440p 30", 2560, 1440, Rational(30)},
        {"QHD 1440p 60", 2560, 1440, Rational(60)},
        {"4K UHD 23.976", 3840, 2160, Rational(24000, 1001)},
        {"4K UHD 24", 3840, 2160, Rational(24)},
        {"4K UHD 25", 3840, 2160, Rational(25)},
        {"4K UHD 30", 3840, 2160, Rational(30)},
        {"4K UHD 60", 3840, 2160, Rational(60)},
        {"DCI 4K 24", 4096, 2160, Rational(24)},
        {"HD 720p 30", 1280, 720, Rational(30)},
        {"HD 720p 60", 1280, 720, Rational(60)},
        {"Vertical 1080x1920 30 (reels, stories)", 1080, 1920, Rational(30)},
        {"Vertical 1080x1920 60", 1080, 1920, Rational(60)},
        {"Square 1080x1080 30", 1080, 1080, Rational(30)},
    };
    return kPresets;
}

const std::vector<Rational>& SequenceDialog::frameRates() {
    static const std::vector<Rational> kRates = {Rational(24000, 1001), Rational(24), Rational(25),
                                                 Rational(30000, 1001), Rational(30), Rational(50),
                                                 Rational(60000, 1001), Rational(60), Rational(120)};
    return kRates;
}

SequenceDialog::SequenceDialog(Mode mode, QWidget* parent) : QDialog(parent), m_mode(mode) {
    setWindowTitle(mode == Mode::Create ? tr("New Sequence") : tr("Sequence Settings"));
    setMinimumWidth(420);
    m_name = new QLineEdit(this);
    m_preset = new QComboBox(this);
    for (const auto& p : presets()) {
        m_preset->addItem(QString::fromLatin1(p.name));
    }
    m_preset->addItem(tr("Custom"));
    m_width = new QSpinBox(this);
    m_height = new QSpinBox(this);
    for (auto* s : {m_width, m_height}) {
        s->setRange(16, 8192);
        s->setSingleStep(2);
        s->setSuffix(tr(" px"));
    }
    m_rate = new QComboBox(this);
    for (const auto& r : frameRates()) {
        m_rate->addItem(rateLabel(r));
    }
    m_sampleRate = new QComboBox(this);
    m_sampleRate->addItem(QStringLiteral("48000 Hz"), 48000);
    m_sampleRate->addItem(QStringLiteral("44100 Hz"), 44100);
    m_sampleRate->addItem(QStringLiteral("96000 Hz"), 96000);
    auto* size = new QHBoxLayout();
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("x"), this));
    size->addWidget(m_height);

    auto* form = new QFormLayout();
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Preset:"), m_preset);
    form->addRow(tr("Frame size:"), size);
    form->addRow(tr("Frame rate:"), m_rate);
    form->addRow(tr("Audio sample rate:"), m_sampleRate);
    if (mode == Mode::Create) {
        m_videoTracks = new QSpinBox(this);
        m_videoTracks->setRange(1, 99);
        m_audioTracks = new QSpinBox(this);
        m_audioTracks->setRange(0, 99);
        form->addRow(tr("Video tracks:"), m_videoTracks);
        form->addRow(tr("Audio tracks:"), m_audioTracks);
        setTrackCounts(3, 3);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_preset, &QComboBox::currentIndexChanged, this, &SequenceDialog::applyPreset);
    for (auto* s : {m_width, m_height}) {
        connect(s, &QSpinBox::valueChanged, this, &SequenceDialog::matchPreset);
    }
    connect(m_rate, &QComboBox::currentIndexChanged, this, &SequenceDialog::matchPreset);
    m_name->setText(tr("Sequence"));
    setPreset(4); // HD 1080p 30
}

void SequenceDialog::setPreset(int index) {
    m_preset->setCurrentIndex(index);
    applyPreset(index);
}

void SequenceDialog::applyPreset(int index) {
    if (m_updating || index < 0 || index >= static_cast<int>(presets().size())) {
        return; // "Custom" keeps the fields
    }
    m_updating = true;
    const auto& p = presets()[static_cast<std::size_t>(index)];
    m_width->setValue(p.width);
    m_height->setValue(p.height);
    const auto& rates = frameRates();
    m_rate->setCurrentIndex(static_cast<int>(std::find(rates.begin(), rates.end(), p.frameRate) - rates.begin()));
    m_updating = false;
}

void SequenceDialog::matchPreset() {
    if (m_updating) {
        return;
    }
    const auto s = settings();
    int match = static_cast<int>(presets().size()); // Custom
    for (std::size_t i = 0; i < presets().size(); ++i) {
        const auto& p = presets()[i];
        if (p.width == s.width && p.height == s.height && p.frameRate == s.frameRate) {
            match = static_cast<int>(i);
            break;
        }
    }
    m_updating = true;
    m_preset->setCurrentIndex(match);
    m_updating = false;
}

void SequenceDialog::setSequence(const std::string& name, const model::SequenceSettings& s) {
    m_name->setText(QString::fromStdString(name));
    m_updating = true;
    m_width->setValue(s.width);
    m_height->setValue(s.height);
    const auto& rates = frameRates();
    const auto it = std::find(rates.begin(), rates.end(), s.frameRate);
    if (it == rates.end()) {
        m_rate->addItem(rateLabel(s.frameRate)); // keep an unusual rate selectable
        m_rate->setCurrentIndex(m_rate->count() - 1);
    } else {
        m_rate->setCurrentIndex(static_cast<int>(it - rates.begin()));
    }
    const int sr = m_sampleRate->findData(s.sampleRate);
    if (sr < 0) {
        m_sampleRate->addItem(QStringLiteral("%1 Hz").arg(s.sampleRate), s.sampleRate);
        m_sampleRate->setCurrentIndex(m_sampleRate->count() - 1);
    } else {
        m_sampleRate->setCurrentIndex(sr);
    }
    m_updating = false;
    matchPreset();
}

void SequenceDialog::setTrackCounts(int video, int audio) {
    if (m_videoTracks != nullptr) {
        m_videoTracks->setValue(video);
        m_audioTracks->setValue(audio);
    }
}

std::string SequenceDialog::name() const {
    const auto text = m_name->text().trimmed();
    return (text.isEmpty() ? tr("Sequence") : text).toStdString();
}

model::SequenceSettings SequenceDialog::settings() const {
    model::SequenceSettings s;
    s.width = m_width->value() - m_width->value() % 2; // encoders need even sizes
    s.height = m_height->value() - m_height->value() % 2;
    const auto& rates = frameRates();
    const int r = m_rate->currentIndex();
    if (r >= 0 && r < static_cast<int>(rates.size())) {
        s.frameRate = rates[static_cast<std::size_t>(r)];
    } else {
        // An unusual rate added by setSequence(): parse "x.yyy fps" back is lossy, so keep 30 as safety.
        const double fps = m_rate->currentText().split(QLatin1Char(' ')).front().toDouble();
        s.frameRate = fps > 0 ? Rational(static_cast<std::int64_t>(fps * 1000.0 + 0.5), 1000) : Rational(30);
    }
    s.sampleRate = m_sampleRate->currentData().toInt();
    return s;
}

int SequenceDialog::videoTracks() const {
    return m_videoTracks != nullptr ? m_videoTracks->value() : 0;
}

int SequenceDialog::audioTracks() const {
    return m_audioTracks != nullptr ? m_audioTracks->value() : 0;
}

AddTracksDialog::AddTracksDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Add Tracks"));
    m_video = new QSpinBox(this);
    m_audio = new QSpinBox(this);
    for (auto* s : {m_video, m_audio}) {
        s->setRange(0, 99);
    }
    m_video->setValue(1);
    m_audio->setValue(1);
    auto* form = new QFormLayout();
    form->addRow(tr("Video tracks to add:"), m_video);
    form->addRow(tr("Audio tracks to add:"), m_audio);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void AddTracksDialog::setCounts(int video, int audio) {
    m_video->setValue(video);
    m_audio->setValue(audio);
}

int AddTracksDialog::videoTracks() const {
    return m_video->value();
}

int AddTracksDialog::audioTracks() const {
    return m_audio->value();
}

} // namespace loki::app

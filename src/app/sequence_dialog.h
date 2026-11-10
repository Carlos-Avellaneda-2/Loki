#pragma once

#include "model/types.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

namespace loki::app {

// Sequence > New Sequence / Sequence Settings: name, preset (HD, 4K UHD, 1440p, vertical, square...),
// frame size, frame rate, audio sample rate and, for a new sequence, how many tracks it starts with.
class SequenceDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Create, Edit };

    struct Preset {
        const char* name;
        int width;
        int height;
        Rational frameRate;
    };
    [[nodiscard]] static const std::vector<Preset>& presets();
    [[nodiscard]] static const std::vector<Rational>& frameRates();

    SequenceDialog(Mode mode, QWidget* parent = nullptr);

    // Fills the fields from an existing sequence (Edit) or a starting point (Create).
    void setSequence(const std::string& name, const model::SequenceSettings& settings);
    void setPreset(int index);
    void setTrackCounts(int video, int audio);

    [[nodiscard]] std::string name() const;
    [[nodiscard]] model::SequenceSettings settings() const;
    [[nodiscard]] int videoTracks() const;
    [[nodiscard]] int audioTracks() const;

private:
    void applyPreset(int index);
    void matchPreset();

    Mode m_mode;
    QLineEdit* m_name = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QComboBox* m_rate = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QSpinBox* m_videoTracks = nullptr;
    QSpinBox* m_audioTracks = nullptr;
    bool m_updating = false;
};

// Sequence > Add Tracks: how many video and audio tracks to append.
class AddTracksDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AddTracksDialog(QWidget* parent = nullptr);
    void setCounts(int video, int audio);
    [[nodiscard]] int videoTracks() const;
    [[nodiscard]] int audioTracks() const;

private:
    QSpinBox* m_video = nullptr;
    QSpinBox* m_audio = nullptr;
};

} // namespace loki::app

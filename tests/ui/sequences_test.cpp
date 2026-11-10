#include "app/main_window.h"
#include "app/sequence_dialog.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"

#include <QAction>
#include <QComboBox>

#include <gtest/gtest.h>

namespace {

using namespace loki;

class SequencesTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
        window = std::make_unique<app::MainWindow>(nullptr, options);
    }
    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& active() { return *controller().project().findSequence(window->activeSequence()); }
};

TEST(SequenceDialogTest, PresetsFillTheSettingsAndCustomSizesStayEven) {
    app::SequenceDialog dialog(app::SequenceDialog::Mode::Create);
    const auto& presets = app::SequenceDialog::presets();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        dialog.setPreset(i);
        const auto s = dialog.settings();
        EXPECT_EQ(s.width, presets[static_cast<std::size_t>(i)].width) << presets[static_cast<std::size_t>(i)].name;
        EXPECT_EQ(s.height, presets[static_cast<std::size_t>(i)].height);
        EXPECT_EQ(s.frameRate, presets[static_cast<std::size_t>(i)].frameRate);
    }
    model::SequenceSettings odd;
    odd.width = 1001;
    odd.height = 777;
    odd.frameRate = Rational(60000, 1001);
    dialog.setSequence("Odd", odd);
    EXPECT_EQ(dialog.settings().width, 1000);
    EXPECT_EQ(dialog.settings().height, 776);
    EXPECT_EQ(dialog.settings().frameRate, Rational(60000, 1001));
    EXPECT_EQ(dialog.name(), "Odd");
    dialog.setTrackCounts(6, 8);
    EXPECT_EQ(dialog.videoTracks(), 6);
    EXPECT_EQ(dialog.audioTracks(), 8);
}

TEST_F(SequencesTest, NewSequencesAreActiveAndSwitchable) {
    const auto first = window->activeSequence();
    model::SequenceSettings uhd;
    uhd.width = 3840;
    uhd.height = 2160;
    uhd.frameRate = Rational(60);
    ASSERT_TRUE(window->createSequence("4K60", uhd, 6, 8));
    EXPECT_NE(window->activeSequence(), first);
    EXPECT_EQ(active().name, "4K60");
    EXPECT_EQ(active().settings.width, 3840);
    EXPECT_EQ(active().videoTracks.size(), 6u);
    EXPECT_EQ(active().audioTracks.size(), 8u);
    EXPECT_EQ(window->programMonitor()->sequenceId(), window->activeSequence());
    // The timeline's switcher lists both and switches back.
    auto* box = window->timeline()->sequenceSelector();
    ASSERT_EQ(box->count(), 2);
    EXPECT_EQ(box->currentText(), QStringLiteral("4K60"));
    box->setCurrentIndex(0);
    EXPECT_EQ(window->activeSequence(), first);
    // Undo removes the new sequence; the first one stays active.
    controller().undoAction()->trigger();
    EXPECT_EQ(controller().project().sequences().size(), 1u);
    EXPECT_EQ(window->activeSequence(), first);
}

TEST_F(SequencesTest, AddTracksAddsManyInOneUndoStep) {
    const auto video = active().videoTracks.size();
    const auto audio = active().audioTracks.size();
    ASSERT_TRUE(window->addTracks(5, 7));
    EXPECT_EQ(active().videoTracks.size(), video + 5);
    EXPECT_EQ(active().audioTracks.size(), audio + 7);
    EXPECT_EQ(active().audioTracks.back().state.name, "A" + std::to_string(audio + 7));
    controller().undoAction()->trigger();
    EXPECT_EQ(active().videoTracks.size(), video);
    EXPECT_EQ(active().audioTracks.size(), audio);
    EXPECT_FALSE(window->addTracks(0, 0));
}

} // namespace

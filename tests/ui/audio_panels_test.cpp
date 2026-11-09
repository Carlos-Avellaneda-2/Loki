#include "app/main_window.h"
#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "model/effect_registry.h"
#include "ui/audio_meters.h"
#include "ui/audio_mixer_panel.h"
#include "ui/effect_controls_panel.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"

#include <QAction>
#include <QDial>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;
using namespace std::chrono_literals;

TEST(LevelMeterTest, BallisticsFallHoldAndClip) {
    ui::LevelMeter meter;
    const auto t0 = std::chrono::steady_clock::time_point(10s);
    audio::StereoLevel loud;
    loud.left = {0.5f, 0.25f};
    loud.right = {1.0f, 0.5f};
    meter.setLevel(loud, t0);
    EXPECT_NEAR(meter.peakDb(0), -6.02, 0.01);
    EXPECT_NEAR(meter.rmsDb(0), -12.04, 0.01);
    EXPECT_TRUE(meter.clipped()); // right reached 0 dBFS
    // Silence: the bar falls at 24 dB/s, the peak marker holds.
    meter.setLevel({}, t0 + 500ms);
    EXPECT_NEAR(meter.peakDb(0), -18.02, 0.01);
    EXPECT_NEAR(meter.holdDb(0), -6.02, 0.01);
    meter.setLevel({}, t0 + 2000ms); // hold expired
    EXPECT_LT(meter.holdDb(0), -6.1);
    meter.resetClip();
    EXPECT_FALSE(meter.clipped());
    meter.setSilent(t0 + 10s);
    EXPECT_EQ(meter.peakDb(0), ui::LevelMeter::kFloorDb);
}

TEST(AudioMixerScaleTest, FaderPositionsRoundTrip) {
    EXPECT_EQ(ui::AudioMixerPanel::faderPosition(ui::AudioMixerPanel::kFaderMaxDb), ui::AudioMixerPanel::kFaderSteps);
    EXPECT_EQ(ui::AudioMixerPanel::faderPosition(audio::kSilenceDb), 0);
    EXPECT_EQ(ui::AudioMixerPanel::faderDb(0), audio::kSilenceDb);
    for (const double db : {-40.0, -20.0, -6.0, 0.0, 3.5}) {
        EXPECT_NEAR(ui::AudioMixerPanel::faderDb(ui::AudioMixerPanel::faderPosition(db)), db, 0.1) << db;
    }
}

class AudioPanelsTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;
    model::MediaId media;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) {
            return std::make_unique<audio::ManualAudioDevice>(std::move(cb));
        };
        window = std::make_unique<app::MainWindow>(nullptr, options);
        model::MediaItem item;
        item.name = "clip";
        item.path = MediaAssets::get().mpeg4WithAudio;
        item.info = media::probeMedia(item.path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        media = cmd->mediaId();
        controller().execute(std::move(cmd));
        window->sourceEdit(media, false); // 2 s A/V at 0 on V1/A1
    }

    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& sequence() { return *controller().project().findSequence(window->activeSequence()); }
};

TEST_F(AudioPanelsTest, MixerHasAStripPerAudioTrackPlusMaster) {
    auto* mixer = window->audioMixer();
    ASSERT_NE(mixer, nullptr);
    const auto tracks = sequence().audioTracks.size();
    ASSERT_EQ(mixer->strips().size(), tracks + 1);
    EXPECT_EQ(mixer->strips().front().track, sequence().audioTracks[0].id);
    EXPECT_FALSE(mixer->strips().back().track);
    controller().execute(std::make_unique<commands::AddTrackCommand>(window->activeSequence(), model::TrackKind::Audio));
    EXPECT_EQ(mixer->strips().size(), tracks + 2);
}

TEST_F(AudioPanelsTest, FaderDragIsOneUndoStepAndFollowsUndo) {
    auto* mixer = window->audioMixer();
    const auto& strip = mixer->strips().front();
    const auto track = strip.track;
    const auto before = controller().undoStack().count();
    strip.fader->setSliderDown(true); // sliderPressed: one gesture
    strip.fader->setValue(ui::AudioMixerPanel::faderPosition(-3.0));
    strip.fader->setValue(ui::AudioMixerPanel::faderPosition(-10.0));
    strip.fader->setSliderDown(false);
    EXPECT_EQ(controller().undoStack().count(), before + 1);
    EXPECT_NEAR(controller().project().findTrack(track).track->state.volumeDb, -10.0, 0.1);
    controller().undoAction()->trigger();
    EXPECT_EQ(controller().project().findTrack(track).track->state.volumeDb, 0.0);
    EXPECT_EQ(mixer->strips().front().fader->value(), ui::AudioMixerPanel::faderPosition(0.0));
    // Master, mute and solo.
    mixer->strips().back().fader->setValue(ui::AudioMixerPanel::faderPosition(-6.0));
    EXPECT_NEAR(sequence().masterVolumeDb, -6.0, 0.1);
    mixer->strips().front().solo->click();
    EXPECT_TRUE(controller().project().findTrack(track).track->state.solo);
    mixer->strips().front().mute->click();
    EXPECT_TRUE(controller().project().findTrack(track).track->state.muted);
}

TEST_F(AudioPanelsTest, EffectControlsShowVolumeAndPannerForAudioClips) {
    auto* panel = window->effectControls();
    panel->setClip(sequence().audioTracks[0].clips[0].id);
    EXPECT_EQ(panel->shownEffects(),
              (std::vector<std::string>{std::string(model::kVolumeEffect), std::string(model::kPannerEffect)}));
    panel->setValue(std::string(model::kVolumeEffect), "level", -9.0, 0);
    EXPECT_EQ(model::resolveAudioMix(sequence().audioTracks[0].clips[0], Rational(1)).levelDb, -9.0);
    panel->setClip(sequence().videoTracks[0].clips[0].id);
    EXPECT_EQ(panel->shownEffects(),
              (std::vector<std::string>{std::string(model::kMotionEffect), std::string(model::kOpacityEffect)}));
}

TEST_F(AudioPanelsTest, TimelineCopyPasteAtThePlayhead) {
    auto* view = window->timeline()->view();
    view->setSelection({sequence().videoTracks[0].clips[0].id});
    view->copySelection();
    EXPECT_EQ(view->clipboard().items.size(), 2u); // with the linked audio
    window->programMonitor()->engine().seek(Rational(3));
    ASSERT_TRUE(window->programMonitor()->engine().waitIdle());
    view->paste(false);
    ASSERT_EQ(sequence().videoTracks[0].clips.size(), 2u);
    EXPECT_EQ(sequence().videoTracks[0].clips[1].start, Rational(3));
    EXPECT_EQ(sequence().audioTracks[0].clips[1].start, Rational(3));
    ASSERT_TRUE(window->programMonitor()->engine().waitIdle());
    EXPECT_EQ(window->programMonitor()->engine().position(), sequence().videoTracks[0].clips[1].end());
    view->setSelection({sequence().videoTracks[0].clips[0].id});
    view->cutSelection();
    EXPECT_EQ(sequence().videoTracks[0].clips.size(), 1u);
    EXPECT_EQ(sequence().audioTracks[0].clips.size(), 1u);
}

} // namespace

#include "app/main_window.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "model/effect_registry.h"
#include "model/transitions.h"
#include "text/title_renderer.h"
#include "ui/effect_controls_panel.h"
#include "ui/effects_panel.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"

#include <QTreeWidget>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

TEST(TitleRendererTest, DrawsTextWithShadowOnTransparentCanvas) {
    text::TitleSpec spec;
    spec.text = "LOKI";
    spec.font = "Arial";
    spec.color = "#ff0000";
    spec.size = 40;
    spec.x = 80;
    spec.y = 30;
    spec.canvasWidth = 160;
    spec.canvasHeight = 60;
    const auto frame = text::renderTitle(spec);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->format, media::PixelFormat::Rgba);
    EXPECT_EQ(frame->width, 160);
    int red = 0;
    int transparent = 0;
    for (std::size_t i = 0; i < frame->planes[0].size(); i += 4) {
        const auto* p = &frame->planes[0][i];
        red += (p[3] == 255 && p[0] > 200 && p[1] < 40) ? 1 : 0;
        transparent += p[3] == 0 ? 1 : 0;
    }
    EXPECT_GT(red, 50);           // the glyphs
    EXPECT_GT(transparent, 4000); // the rest of the canvas
    EXPECT_EQ(frame->planes[0][3], 0); // corner untouched
}

class TitlesEffectsTest : public ::testing::Test {
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
        seek(Rational(2));
        window->sourceEdit(media, false); // and again at 2 s: a cut at 2 s
    }

    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& sequence() { return *controller().project().findSequence(window->activeSequence()); }
    void seek(Rational t) {
        window->programMonitor()->engine().seek(t);
        ASSERT_TRUE(window->programMonitor()->engine().waitIdle());
    }
};

TEST_F(TitlesEffectsTest, NewTitleIsSelectedAndEditableAndRenders) {
    seek(Rational(1));
    window->newTitle();
    // V1 holds the footage (targeted): the title goes on V2.
    const auto& top = sequence().videoTracks[1];
    ASSERT_EQ(top.clips.size(), 1u);
    const auto& title = top.clips[0];
    EXPECT_EQ(title.start, sequence().settings.frameRate.isPositive()
                               ? quantizeToFrame(Rational(1), sequence().settings.frameRate, Rounding::Floor)
                               : Rational(1));
    auto* panel = window->effectControls();
    EXPECT_EQ(panel->clip(), title.id);
    EXPECT_EQ(panel->shownEffects(), (std::vector<std::string>{std::string(model::kMotionEffect),
                                                               std::string(model::kOpacityEffect),
                                                               std::string(model::kTextEffect)}));
    panel->setString(std::string(model::kTextEffect), "text", "Hola");
    EXPECT_EQ(model::effectiveString(sequence().videoTracks[1].clips[0], model::kTextEffect, "text"), "Hola");
    // The Program Monitor composites the title over V1.
    window->programMonitor()->publishSnapshot();
    seek(Rational(3, 2));
    auto& engine = window->programMonitor()->engine();
    (void)engine.currentFrame();
    const auto picture = engine.currentPicture();
    ASSERT_TRUE(picture && picture->composite);
    ASSERT_EQ(picture->composite->layers.size(), 2u);
    EXPECT_EQ(picture->composite->layers[1].frame->format, media::PixelFormat::Rgba);
}

TEST_F(TitlesEffectsTest, EffectsPanelAppliesEffectsAndTransitions) {
    auto* view = window->timeline()->view();
    const auto first = sequence().videoTracks[0].clips[0].id;
    view->setSelection({first});
    auto* effects = window->effectsPanel();
    ASSERT_NE(effects, nullptr);
    emit effects->applyRequested(QString::fromLatin1(model::kColorEffect.data()), false);
    EXPECT_NE(model::findEffectByType(sequence().videoTracks[0].clips[0], model::kColorEffect), nullptr);
    window->effectControls()->setClip(first);
    const auto shown = window->effectControls()->shownEffects();
    EXPECT_EQ(shown.back(), std::string(model::kColorEffect));
    // Ctrl+D equivalent at the cut (2 s) on the targeted video track.
    seek(Rational(2));
    emit effects->applyRequested(QString::fromLatin1(model::kCrossDissolve.data()), true);
    ASSERT_EQ(sequence().videoTracks[0].transitions.size(), 1u);
    const auto& t = sequence().videoTracks[0].transitions[0];
    EXPECT_EQ(t.from, first);
    EXPECT_EQ(t.to, sequence().videoTracks[0].clips[1].id);
    // Selecting it shows it in Effect Controls; Delete removes it.
    view->selectTransition(t.id);
    EXPECT_EQ(window->effectControls()->transition(), t.id);
    view->deleteSelection();
    EXPECT_TRUE(sequence().videoTracks[0].transitions.empty());
    // Drag payload from the panel's tree.
    EXPECT_FALSE(effects->tree()->findItems(QStringLiteral("Cross Dissolve"), Qt::MatchRecursive).isEmpty());
    EXPECT_FALSE(view->dropEffect(QByteArray("transition:loki.dipToBlack"), QPoint(0, 0))); // not on a clip
}

} // namespace

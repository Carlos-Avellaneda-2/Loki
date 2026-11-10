// Phase 15 model and commands: captions + SRT (D40), track automation (D39), multicam (D43),
// format v5.
#include "commands/caption_commands.h"
#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "commands/undo_stack.h"
#include "model/project_json.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;
using loki::test::makeMedia;

class Phase15Test : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};

    void pushVerified(std::unique_ptr<Command> command) {
        ASSERT_NE(command, nullptr);
        const auto before = dump(project);
        stack.push(std::move(command));
        ASSERT_TRUE(project.validate().empty()) << project.validate().front();
        const auto after = dump(project);
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }
    const Sequence& seq() { return *project.findSequence(ids.sequence); }
};

TEST_F(Phase15Test, CaptionsAreAddedWithoutOverlapEditedAndRemoved) {
    CaptionId first;
    pushVerified(makeAddCaption(project, ids.sequence, Rational(5), Rational(3), "Hello", &first));
    pushVerified(makeAddCaption(project, ids.sequence, Rational(2), Rational(10), "Before"));
    ASSERT_EQ(seq().captions.size(), 2u);
    EXPECT_EQ(seq().captions[0].text, "Before");
    EXPECT_EQ(seq().captions[0].duration, Rational(3)); // stopped at the next caption
    EXPECT_THROW((void)makeAddCaption(project, ids.sequence, Rational(6), Rational(1), "inside"), CommandError);
    auto edited = seq().captions[1];
    edited.text = "Hello\nworld";
    pushVerified(makeEditCaption(project, ids.sequence, edited));
    EXPECT_EQ(seq().captions[1].text, "Hello\nworld");
    edited.start = Rational(3); // would overlap "Before"
    EXPECT_THROW((void)makeEditCaption(project, ids.sequence, edited), CommandError);
    auto style = seq().captionStyle;
    style.size = 72;
    style.backgroundOpacity = 0;
    pushVerified(makeSetCaptionStyle(project, ids.sequence, style));
    EXPECT_EQ(seq().captionStyle.size, 72.0);
    pushVerified(makeRemoveCaption(project, ids.sequence, first));
    EXPECT_EQ(seq().captions.size(), 1u);
    EXPECT_NE(captionAt(seq().captions, Rational(3)), nullptr);
    EXPECT_EQ(captionAt(seq().captions, Rational(9)), nullptr);
}

TEST(SrtTest, ParsesAndFormatsSubRip) {
    const std::string text =
        "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\nFirst line\r\nSecond line\r\n\r\n"
        "2\n00:00:03.250 --> 00:00:04,000 X1:10\nNext\n\n";
    const auto captions = parseSrt(text);
    ASSERT_EQ(captions.size(), 2u);
    EXPECT_EQ(captions[0].start, Rational(1));
    EXPECT_EQ(captions[0].duration, Rational(3, 2));
    EXPECT_EQ(captions[0].text, "First line\nSecond line");
    EXPECT_EQ(captions[1].start, Rational(13, 4));
    const auto back = parseSrt(formatSrt(captions));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[1].text, "Next");
    EXPECT_EQ(back[1].duration, Rational(3, 4));
    EXPECT_NE(formatSrt(captions).find("00:00:03,250 --> 00:00:04,000"), std::string::npos);
    EXPECT_THROW((void)parseSrt("1\n00:00:05,000 --> 00:00:04,000\nbad\n"), CommandError);
    EXPECT_THROW((void)parseSrt("1\nnot a timing\n"), CommandError);
}

TEST_F(Phase15Test, TrackAutomationKeysDriveTheFader) {
    const auto a1 = ids.a1;
    pushVerified(setTrackAutomationValue(project, a1, AutomationKind::Volume, Rational(1), -6.0));
    pushVerified(setTrackAutomationValue(project, a1, AutomationKind::Volume, Rational(3), -18.0));
    const auto& state = project.findTrack(a1).track->state;
    EXPECT_EQ(state.volumeAutomation.size(), 2u);
    EXPECT_NEAR(state.volumeAt(Rational(2)), -12.0, 1e-9);
    EXPECT_NEAR(state.volumeAt(Rational(0)), -6.0, 1e-9);
    pushVerified(moveTrackAutomationKey(project, a1, AutomationKind::Volume, Rational(3), Rational(5)));
    EXPECT_NEAR(project.findTrack(a1).track->state.volumeAt(Rational(3)), -12.0, 1e-9);
    pushVerified(toggleTrackAutomationKey(project, a1, AutomationKind::Pan, Rational(2)));
    EXPECT_EQ(project.findTrack(a1).track->state.panAutomation.size(), 1u);
    pushVerified(clearTrackAutomation(project, a1, AutomationKind::Volume));
    EXPECT_TRUE(project.findTrack(a1).track->state.volumeAutomation.empty());
    EXPECT_THROW((void)toggleTrackAutomationKey(project, ids.v1, AutomationKind::Volume, Rational(1)), CommandError);
}

TEST_F(Phase15Test, MulticamSourceAndAngleSwitching) {
    auto second = makeMedia("cam2", Rational(30));
    stack.push(std::make_unique<AddMediaCommand>(second));
    MediaId multicam;
    SequenceId source;
    pushVerified(makeMulticamSource(project, {ids.media, second.id}, "Multi", seq().settings, &multicam, &source));
    const auto* media = project.findMedia(multicam);
    ASSERT_NE(media, nullptr);
    EXPECT_TRUE(media->multicam);
    const auto* src = project.findSequence(source);
    ASSERT_EQ(src->videoTracks.size(), 2u);
    EXPECT_EQ(src->videoTracks[1].clips[0].media, second.id);
    EXPECT_EQ(src->audioTracks[0].clips.size(), 1u);
    // Place it and cut between cameras.
    auto clip = makeClip(multicam, Rational(0), Rational(20));
    stack.push(std::make_unique<AddClipCommand>(ids.v1, clip));
    pushVerified(makeSwitchAngle(project, ids.sequence, clip.id, Rational(4), 1));
    const auto& clips = project.findTrack(ids.v1).track->clips;
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0].angle, 0);
    EXPECT_EQ(clips[1].angle, 1);
    EXPECT_EQ(clips[1].start, Rational(4));
    EXPECT_EQ(clips[1].sourceIn, Rational(4));
    EXPECT_THROW((void)makeSwitchAngle(project, ids.sequence, clips[1].id, Rational(10), 5), CommandError);
    EXPECT_THROW((void)makeMulticamSource(project, {ids.media}, "One", seq().settings), CommandError);
}

TEST_F(Phase15Test, Version5RoundTripsAndOlderFilesLoad) {
    pushVerified(makeAddCaption(project, ids.sequence, Rational(1), Rational(2), "Hola"));
    pushVerified(setTrackAutomationValue(project, ids.a1, AutomationKind::Pan, Rational(1), 40.0));
    auto clip = makeClip(ids.media, Rational(0), Rational(4));
    clip.maintainPitch = true;
    clip.speed = Rational(2);
    stack.push(std::make_unique<AddClipCommand>(ids.v1, clip));
    const auto text = serializeProject(project);
    EXPECT_NE(text.find("\"version\": 5"), std::string::npos);
    EXPECT_EQ(parseProject(text), project);
    const auto golden = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v5.loki");
    const auto& s = golden.sequences()[0];
    ASSERT_EQ(s.captions.size(), 2u);
    EXPECT_EQ(s.captions[1].text, "Second");
    EXPECT_EQ(s.captionStyle.size, 60.0);
    EXPECT_EQ(s.audioTracks[0].state.volumeAutomation.size(), 2u);
    EXPECT_TRUE(s.videoTracks[0].clips[0].maintainPitch);
    EXPECT_EQ(s.videoTracks[0].clips[0].angle, 1);
    EXPECT_TRUE(golden.findMedia(s.videoTracks[0].clips[0].media)->multicam);
    for (const char* older : {"project_v1.loki", "project_v2.loki", "project_v3.loki", "project_v4.loki"}) {
        EXPECT_NO_THROW((void)loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / older)) << older;
    }
}

} // namespace

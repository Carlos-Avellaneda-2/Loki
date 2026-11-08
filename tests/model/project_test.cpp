#include "model/project.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::model;
using loki::test::makeClip;
using loki::test::makeMedia;

TEST(ProjectTest, DefaultProjectHasOneSequenceWithThreeAndThreeTracks) {
    const auto project = makeDefaultProject();
    ASSERT_EQ(project.sequences().size(), 1u);
    const auto& seq = project.sequences().front();
    EXPECT_EQ(seq.name, "Sequence 01");
    EXPECT_EQ(seq.settings.frameRate, Rational(30));
    ASSERT_EQ(seq.videoTracks.size(), 3u);
    ASSERT_EQ(seq.audioTracks.size(), 3u);
    EXPECT_EQ(seq.videoTracks[0].state.name, "V1");
    EXPECT_EQ(seq.audioTracks[2].state.name, "A3");
    EXPECT_EQ(seq.videoTracks[1].kind, TrackKind::Video);
    EXPECT_TRUE(project.validate().empty());
}

TEST(ProjectTest, BuilderSortsNothingButValidatesClips) {
    auto media = makeMedia("a");
    auto seq = makeDefaultSequence("S", {}, 1, 0);
    seq.videoTracks[0].clips.push_back(makeClip(media.id, Rational(0), Rational(5)));
    seq.videoTracks[0].clips.push_back(makeClip(media.id, Rational(5), Rational(5)));
    ProjectBuilder builder;
    builder.addMedia(media).addSequence(seq);
    const auto project = builder.build();
    EXPECT_EQ(project.sequences()[0].duration(), Rational(10));
}

TEST(ProjectTest, BuilderRejectsOverlappingClips) {
    auto media = makeMedia("a");
    auto seq = makeDefaultSequence("S", {}, 1, 0);
    seq.videoTracks[0].clips.push_back(makeClip(media.id, Rational(0), Rational(5)));
    seq.videoTracks[0].clips.push_back(makeClip(media.id, Rational(4), Rational(5)));
    ProjectBuilder builder;
    builder.addMedia(media);
    EXPECT_THROW(builder.addSequence(seq), ModelError);
}

TEST(ProjectTest, BuilderRejectsUnknownMediaAndBadShapes) {
    ProjectBuilder builder;
    auto seq = makeDefaultSequence("S", {}, 1, 0);
    seq.videoTracks[0].clips.push_back(makeClip(MediaId::generate(), Rational(0), Rational(5)));
    EXPECT_THROW(builder.addSequence(seq), ModelError);

    auto media = makeMedia("a");
    builder.addMedia(media);
    auto zero = makeDefaultSequence("S", {}, 1, 0);
    zero.videoTracks[0].clips.push_back(makeClip(media.id, Rational(0), Rational(0)));
    EXPECT_THROW(builder.addSequence(zero), ModelError);

    auto negative = makeDefaultSequence("S", {}, 1, 0);
    negative.videoTracks[0].clips.push_back(makeClip(media.id, Rational(-1), Rational(1)));
    EXPECT_THROW(builder.addSequence(negative), ModelError);

    auto badSettings = makeDefaultSequence("S", {}, 0, 0);
    badSettings.settings.frameRate = Rational(0);
    EXPECT_THROW(builder.addSequence(badSettings), ModelError);
}

TEST(ProjectTest, BuilderRejectsDuplicateIds) {
    auto media = makeMedia("a");
    ProjectBuilder builder;
    builder.addMedia(media);
    EXPECT_THROW(builder.addMedia(media), ModelError);

    auto seq = makeDefaultSequence("S", {}, 2, 0);
    seq.videoTracks[1].id = seq.videoTracks[0].id;
    EXPECT_THROW(builder.addSequence(seq), ModelError);

    auto seq2 = makeDefaultSequence("S", {}, 2, 0);
    auto clip = makeClip(media.id, Rational(0), Rational(1));
    seq2.videoTracks[0].clips.push_back(clip);
    seq2.videoTracks[1].clips.push_back(clip);
    EXPECT_THROW(builder.addSequence(seq2), ModelError);
}

TEST(ProjectTest, BuilderRejectsWrongTrackKind) {
    ProjectBuilder builder;
    auto seq = makeDefaultSequence("S", {}, 1, 0);
    seq.videoTracks[0].kind = TrackKind::Audio;
    EXPECT_THROW(builder.addSequence(seq), ModelError);
}

TEST(ProjectTest, BuilderRejectsBinsWithUnknownParent) {
    ProjectBuilder builder;
    Bin orphan{BinId::generate(), "orphan", BinId::generate()};
    EXPECT_THROW(builder.addBin(orphan), ModelError);
    auto media = makeMedia("a");
    media.bin = BinId::generate();
    EXPECT_THROW(builder.addMedia(media), ModelError);
}

TEST(ProjectTest, LookupsFindEverything) {
    ProjectBuilder builder;
    auto media = makeMedia("x");
    const Bin bin{BinId::generate(), "Footage", {}};
    media.bin = bin.id;
    auto seq = makeDefaultSequence("S", {}, 2, 1);
    const auto clip = makeClip(media.id, Rational(3), Rational(2));
    seq.audioTracks[0].clips.push_back(clip);
    builder.addBin(bin).addMedia(media).addSequence(seq);
    const auto project = builder.build();

    EXPECT_EQ(project.findMedia(media.id)->name, "x");
    EXPECT_EQ(project.findBin(bin.id)->name, "Footage");
    EXPECT_EQ(project.findSequence(seq.id)->name, "S");
    const auto track = project.findTrack(seq.videoTracks[1].id);
    ASSERT_NE(track.track, nullptr);
    EXPECT_EQ(track.index, 1u);
    EXPECT_EQ(track.sequence->id, seq.id);
    const auto ref = project.findClip(clip.id);
    ASSERT_NE(ref.clip, nullptr);
    EXPECT_EQ(ref.track->id, seq.audioTracks[0].id);
    EXPECT_EQ(ref.clip->end(), Rational(5));
    EXPECT_EQ(project.clipsUsingMedia(media.id), std::vector<ClipId>{clip.id});

    EXPECT_EQ(project.findMedia(MediaId::generate()), nullptr);
    EXPECT_EQ(project.findTrack(TrackId::generate()).track, nullptr);
    EXPECT_EQ(project.findClip(ClipId::generate()).clip, nullptr);
    EXPECT_TRUE(project.binIsWithin(bin.id, bin.id));
    EXPECT_FALSE(project.binIsWithin(BinId(), bin.id));
}

TEST(ProjectTest, CopyIsDeepAndEqual) {
    test::BasicProject basic;
    const auto project = basic.build();
    const Project copy = project; // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy, project);
    Project moved = Project(project);
    EXPECT_EQ(moved, project);
}

TEST(ProjectTest, ClipTimeHelpers) {
    auto clip = makeClip(MediaId::generate(), Rational(10), Rational(5), Rational(2));
    EXPECT_EQ(clip.end(), Rational(15));
    EXPECT_EQ(clip.sourceOut(), Rational(7));
    EXPECT_EQ(clip.toSourceTime(Rational(12)), Rational(4));
}

} // namespace

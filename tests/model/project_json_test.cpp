#include "model/project_json.h"

#include "model_fixtures.h"
#include "test_temp_dir.h"
#include "util/atomic_file.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <fstream>

namespace {

using namespace loki;
using namespace loki::model;
using loki::test::makeClip;
using loki::test::makeMedia;
using nlohmann::json;

Project richProject() {
    ProjectBuilder builder;
    const Bin root{BinId::generate(), "Footage", {}};
    const Bin nested{BinId::generate(), "Entrevistas \xc3\xb1", root.id};
    auto media = makeMedia("interview");
    media.bin = nested.id;
    media.proxyPath = std::filesystem::path("C:/proxies/interview_720p.mp4");
    auto audioOnly = makeMedia("music");
    audioOnly.info.hasVideo = false;
    audioOnly.info.width = 0;
    audioOnly.info.height = 0;
    audioOnly.info.frameRate = Rational();
    audioOnly.info.videoCodec.clear();

    SequenceSettings settings;
    settings.frameRate = Rational(30000, 1001);
    settings.width = 3840;
    settings.height = 2160;
    auto seq = makeDefaultSequence("Main", settings, 2, 2);
    auto clip = makeClip(media.id, Rational(1001, 30000), Rational(5), Rational(1, 3));
    clip.link = LinkId::generate();
    clip.enabled = false;
    auto effect = test::makeOpacityEffect();
    Keyframe k1{Rational(0), 0.0, Interpolation::Bezier, 0.25, 0.75};
    Keyframe k2{Rational(1001, 1000), 100.0, Interpolation::Hold, 1.0 / 3.0, 1.0 / 3.0};
    effect.parameters[0].keyframes = {k1, k2};
    effect.parameters[1].value = 0.1 + 0.2; // non-representable decimal must round-trip exactly
    clip.effects.push_back(effect);
    seq.videoTracks[1].clips.push_back(clip);
    seq.videoTracks[1].state.locked = true;
    auto music = makeClip(audioOnly.id, Rational(0), Rational(60));
    music.link = clip.link;
    seq.audioTracks[0].clips.push_back(music);
    seq.audioTracks[1].state.muted = true;
    seq.videoTracks[0].state.hidden = true;

    builder.addBin(root).addBin(nested).addMedia(media).addMedia(audioOnly).addSequence(seq);
    builder.addSequence(makeDefaultSequence("Empty", {}, 0, 0));
    return builder.build();
}

TEST(ProjectJsonTest, RoundTripPreservesEverything) {
    const auto project = richProject();
    const auto text = serializeProject(project);
    const auto loaded = parseProject(text);
    EXPECT_EQ(loaded, project);
    EXPECT_EQ(serializeProject(loaded), text); // canonical: stable output
}

TEST(ProjectJsonTest, DocumentShape) {
    const auto project = richProject();
    const auto j = toJson(project);
    EXPECT_EQ(j.at("format"), "loki-project");
    EXPECT_EQ(j.at("version"), kProjectFormatVersion);
    const auto& seq = j.at("sequences")[0];
    EXPECT_EQ(seq.at("settings").at("frameRate"), json::array({30000, 1001}));
    const auto& clip = seq.at("videoTracks")[1].at("clips")[0];
    EXPECT_EQ(clip.at("start"), json::array({1001, 30000}));
    EXPECT_EQ(clip.at("sourceIn"), json::array({1, 3}));
    EXPECT_TRUE(clip.at("id").is_string());
    EXPECT_EQ(clip.at("id").get<std::string>().size(), 36u);
    EXPECT_EQ(clip.at("effects")[0].at("parameters")[0].at("keyframes")[0].at("interpolation"), "bezier");
    EXPECT_TRUE(j.at("bins")[0].at("parent").is_null());
}

TEST(ProjectJsonTest, PathsAreUtf8WithForwardSlashes) {
    ProjectBuilder builder;
    auto media = makeMedia("x");
    media.path = std::filesystem::path(u8"D:/vídeos/año/clip ñ.mp4");
    builder.addMedia(media);
    const auto project = builder.build();
    const auto j = toJson(project);
    EXPECT_EQ(j.at("media")[0].at("path").get<std::string>(), "D:/v\xc3\xad" "deos/a\xc3\xb1o/clip \xc3\xb1.mp4");
    EXPECT_EQ(parseProject(serializeProject(project)), project);
}

TEST(ProjectJsonTest, SaveAndLoadFile) {
    test::TempDir dir;
    const auto file = dir.path() / std::filesystem::path(u8"mi proyecto ñ.loki");
    const auto project = richProject();
    saveProjectFile(project, file);
    EXPECT_EQ(loadProjectFile(file), project);
}

TEST(ProjectJsonTest, RelinksMediaWhenProjectFolderMoved) {
    test::TempDir dir;
    const auto oldRoot = dir.path() / "old";
    const auto newRoot = dir.path() / "new";
    std::filesystem::create_directories(oldRoot / "media");
    util::writeFileAtomic(oldRoot / "media" / "a.mp4", "fake");

    ProjectBuilder builder;
    auto media = makeMedia("a");
    media.path = oldRoot / "media" / "a.mp4";
    builder.addMedia(media);
    const auto project = builder.build();
    saveProjectFile(project, oldRoot / "p.loki");
    EXPECT_EQ(toJson(project, oldRoot / "p.loki").at("media")[0].at("relativePath"), "media/a.mp4");

    std::filesystem::rename(oldRoot, newRoot);
    const auto loaded = loadProjectFile(newRoot / "p.loki");
    EXPECT_EQ(loaded.media()[0].path, (newRoot / "media" / "a.mp4").lexically_normal());
}

TEST(ProjectJsonTest, KeepsAbsolutePathWhenItStillExists) {
    test::TempDir dir;
    util::writeFileAtomic(dir.path() / "a.mp4", "fake");
    ProjectBuilder builder;
    auto media = makeMedia("a");
    media.path = dir.path() / "a.mp4";
    builder.addMedia(media);
    const auto project = builder.build();
    std::filesystem::create_directories(dir.path() / "sub");
    saveProjectFile(project, dir.path() / "sub" / "p.loki");
    EXPECT_EQ(loadProjectFile(dir.path() / "sub" / "p.loki").media()[0].path, media.path);
}

TEST(ProjectJsonTest, LoadsGoldenVersion1File) {
    const auto project = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v1.loki");
    ASSERT_EQ(project.media().size(), 1u);
    EXPECT_EQ(project.media()[0].name, "interview");
    EXPECT_EQ(project.media()[0].id.toString(), "11111111-1111-4111-8111-111111111111");
    EXPECT_EQ(project.media()[0].info.frameRate, Rational(30000, 1001));
    ASSERT_EQ(project.bins().size(), 1u);
    EXPECT_EQ(project.media()[0].bin, project.bins()[0].id);
    ASSERT_EQ(project.sequences().size(), 1u);
    const auto& seq = project.sequences()[0];
    EXPECT_EQ(seq.settings.frameRate, Rational(30000, 1001));
    ASSERT_EQ(seq.videoTracks.size(), 1u);
    ASSERT_EQ(seq.audioTracks.size(), 1u);
    EXPECT_TRUE(seq.audioTracks[0].state.muted);
    ASSERT_EQ(seq.videoTracks[0].clips.size(), 1u);
    const auto& clip = seq.videoTracks[0].clips[0];
    EXPECT_EQ(clip.start, Rational(1001, 30000));
    EXPECT_EQ(clip.duration, Rational(10));
    ASSERT_EQ(clip.effects.size(), 1u);
    const auto* opacity = clip.effects[0].findParameter("opacity");
    ASSERT_NE(opacity, nullptr);
    EXPECT_DOUBLE_EQ(opacity->valueAt(Rational(1, 2)), 50.0);
}

// --- Error handling -------------------------------------------------------------------------

json validDocument() {
    return toJson(richProject());
}

void expectFormatError(const json& j, const std::string& expectedFragment) {
    try {
        (void)fromJson(j);
        FAIL() << "expected ProjectFormatError containing '" << expectedFragment << "'";
    } catch (const ProjectFormatError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

TEST(ProjectJsonTest, RejectsNonJson) {
    EXPECT_THROW((void)parseProject("{ not json"), ProjectFormatError);
    EXPECT_THROW((void)parseProject(""), ProjectFormatError);
}

TEST(ProjectJsonTest, RejectsWrongFormatAndVersions) {
    expectFormatError(json::array(), "not a JSON object");
    auto j = validDocument();
    j["format"] = "something-else";
    expectFormatError(j, "Not a Loki project");
    j = validDocument();
    j.erase("version");
    expectFormatError(j, "version");
    j = validDocument();
    j["version"] = 0;
    expectFormatError(j, "version 0");
    j = validDocument();
    j["version"] = kProjectFormatVersion + 1;
    expectFormatError(j, "newer Loki");
}

TEST(ProjectJsonTest, ReportsPathOfMissingField) {
    auto j = validDocument();
    j["sequences"][0]["videoTracks"][1]["clips"][0].erase("start");
    expectFormatError(j, "/sequences/0/videoTracks/1/clips/0/start");
}

TEST(ProjectJsonTest, RejectsBadValues) {
    auto j = validDocument();
    j["sequences"][0]["videoTracks"][1]["clips"][0]["duration"] = json::array({1, 0});
    expectFormatError(j, "duration");

    j = validDocument();
    j["sequences"][0]["videoTracks"][1]["clips"][0]["start"] = "1/2";
    expectFormatError(j, "[numerator, denominator]");

    j = validDocument();
    j["media"][0]["id"] = "not-a-uuid";
    expectFormatError(j, "/media/0/id");

    j = validDocument();
    j["sequences"][0]["videoTracks"][1]["clips"][0]["effects"][0]["parameters"][0]["keyframes"][0]["interpolation"] =
        "cubic";
    expectFormatError(j, "unknown interpolation");

    j = validDocument();
    j["sequences"][0]["settings"]["width"] = "wide";
    expectFormatError(j, "expected an integer");

    j = validDocument();
    j["media"][0]["info"]["hasVideo"] = 1;
    expectFormatError(j, "expected a boolean");
}

TEST(ProjectJsonTest, RejectsInvariantViolations) {
    auto j = validDocument();
    auto clip = j["sequences"][0]["videoTracks"][1]["clips"][0];
    clip["id"] = ClipId::generate().toString();
    j["sequences"][0]["videoTracks"][1]["clips"].push_back(clip); // overlaps the original
    expectFormatError(j, "overlap");

    j = validDocument();
    j["sequences"][0]["videoTracks"][1]["clips"][0]["media"] = MediaId::generate().toString();
    expectFormatError(j, "unknown media");
}

TEST(ProjectJsonTest, MigrationLeavesCurrentVersionUntouched) {
    auto j = validDocument();
    const auto before = j;
    EXPECT_EQ(migrateProjectJson(j), kProjectFormatVersion);
    EXPECT_EQ(j, before);
}

TEST(ProjectJsonTest, LoadMissingFileThrowsSystemError) {
    test::TempDir dir;
    EXPECT_THROW((void)loadProjectFile(dir.path() / "missing.loki"), std::system_error);
}

} // namespace

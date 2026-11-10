// Phase 14 model: masks (D34), curves (D33), analysis samples (D36) and format v4.
#include "model/project.h"
#include "model/project_json.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace loki;
using namespace loki::model;
using loki::test::makeClip;
using loki::test::makeMedia;

Mask squareMask() {
    Mask m;
    m.id = MaskId::generate();
    m.name = "Mask (1)";
    m.path = rectanglePath(100, 100, 200, 100);
    return m;
}

TEST(MaskModelTest, ShapesAndAnimatedPaths) {
    const auto e = ellipsePath(50, 50, 20, 10);
    ASSERT_EQ(e.size(), 4u);
    EXPECT_EQ(e[0].x, 50.0);
    EXPECT_EQ(e[0].y, 40.0);
    auto m = squareMask();
    EXPECT_TRUE(maskProblem(m).empty());
    m.keyframes = {{Rational(0), rectanglePath(0, 0, 10, 10)}, {Rational(2), rectanglePath(20, 0, 10, 10)}};
    const auto mid = m.pathAt(Rational(1));
    EXPECT_EQ(mid[0].x, 10.0);
    EXPECT_EQ(m.pathAt(Rational(5))[0].x, 20.0);
    m.keyframes[1].path.pop_back();
    EXPECT_FALSE(maskProblem(m).empty()); // vertex counts differ
    const auto moved = transformedPath(rectanglePath(0, 0, 10, 10), 5, 0, 2.0, 5, 5);
    EXPECT_EQ(moved[0].x, 0.0);  // (0 - 5) * 2 + 5 + 5
    EXPECT_EQ(moved[2].x, 20.0); // (10 - 5) * 2 + 5 + 5
}

TEST(CurveModelTest, MonotoneSplineThroughThePoints) {
    EXPECT_NEAR(evaluateCurve(identityCurve(), 0.37), 0.37, 1e-9);
    const std::vector<CurvePoint> s{{0, 0}, {0.25, 0.15}, {0.75, 0.85}, {1, 1}};
    EXPECT_TRUE(curveProblem(s).empty());
    EXPECT_NEAR(evaluateCurve(s, 0.25), 0.15, 1e-9);
    EXPECT_NEAR(evaluateCurve(s, 0.75), 0.85, 1e-9);
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double y = evaluateCurve(s, i / 100.0);
        EXPECT_GE(y, prev - 1e-12); // monotone
        prev = y;
    }
    EXPECT_FALSE(curveProblem({{0, 0}}).empty());
    EXPECT_FALSE(curveProblem({{0.5, 0}, {0.2, 1}}).empty());
}

TEST(FormatV4Test, MasksCurvesAndSamplesRoundTrip) {
    ProjectBuilder builder;
    auto media = makeMedia("a");
    auto seq = makeDefaultSequence("S");
    auto clip = makeClip(media.id, Rational(0), Rational(4));
    Effect lumetri;
    lumetri.id = EffectId::generate();
    lumetri.type = "loki.lumetri";
    lumetri.curves["master"] = {{0, 0}, {0.5, 0.6}, {1, 1}};
    auto mask = squareMask();
    mask.inverted = true;
    mask.mode = MaskMode::Subtract;
    mask.feather.value = 25.0;
    mask.keyframes = {{Rational(1), rectanglePath(0, 0, 50, 50)}, {Rational(3, 2), ellipsePath(30, 30, 20, 20)}};
    lumetri.masks.push_back(mask);
    Effect stabilizer;
    stabilizer.id = EffectId::generate();
    stabilizer.type = "loki.stabilizer";
    stabilizer.samples = {1, 30, 1, 0, 1, 1920, 1080, 1, 0.5, -0.25, 0.001, 1.0};
    clip.effects = {lumetri, stabilizer};
    seq.videoTracks[0].clips.push_back(clip);
    builder.addMedia(media).addSequence(seq);
    const auto project = builder.build();
    const auto text = serializeProject(project);
    EXPECT_NE(text.find("\"version\": 4"), std::string::npos);
    EXPECT_EQ(parseProject(text), project);
}

TEST(FormatV4Test, InvalidMasksAndCurvesAreRejected) {
    ProjectBuilder builder;
    auto media = makeMedia("a");
    auto seq = makeDefaultSequence("S");
    auto clip = makeClip(media.id, Rational(0), Rational(4));
    Effect e;
    e.id = EffectId::generate();
    e.type = "loki.lumetri";
    e.curves["red"] = {{0.5, 0.5}};
    clip.effects.push_back(e);
    seq.videoTracks[0].clips.push_back(clip);
    builder.addMedia(media);
    EXPECT_THROW(builder.addSequence(seq), ModelError);
}

TEST(FormatV4Test, LoadsGoldenVersion4File) {
    const auto project = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v4.loki");
    const auto& clip = project.sequences()[0].videoTracks[0].clips[0];
    ASSERT_EQ(clip.effects.size(), 2u);
    const auto& lumetri = clip.effects[0];
    EXPECT_EQ(lumetri.type, "loki.lumetri");
    ASSERT_EQ(lumetri.masks.size(), 1u);
    EXPECT_EQ(lumetri.masks[0].name, "Face");
    EXPECT_EQ(lumetri.masks[0].feather.value, 40.0);
    ASSERT_EQ(lumetri.masks[0].keyframes.size(), 2u);
    EXPECT_EQ(lumetri.curves.at("master").size(), 3u);
    EXPECT_EQ(clip.effects[1].samples.size(), 12u);
    EXPECT_NO_THROW((void)loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v3.loki"));
}

} // namespace

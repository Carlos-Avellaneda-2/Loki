// Phase 14 playback: effects resolved into layers (Lumetri, masks, key, blur, stabilizer) and the
// new transitions.
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "model/transitions.h"
#include "playback/effect_resolve.h"
#include "playback/sources.h"

#include "media_assets.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::playback;
using loki::test::MediaAssets;

model::MediaItem mediaItem(const std::filesystem::path& path) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = path.stem().string();
    m.path = path;
    m.info = media::probeMedia(path);
    return m;
}

model::Clip clip(const model::MediaItem& media, Rational start, Rational duration) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media.id;
    c.start = start;
    c.duration = duration;
    return c;
}

void setParam(model::Effect& e, const std::string& key, double value) {
    for (auto& p : e.parameters) {
        if (p.key == key) {
            p.value = value;
            return;
        }
    }
    e.parameters.push_back({key, value, {}});
}

class EffectsSourceTest : public ::testing::Test {
protected:
    model::MediaItem video = mediaItem(MediaAssets::get().mpeg4WithAudio); // 25 fps, 2 s

    std::shared_ptr<SequenceSnapshot> snapshot() {
        auto s = std::make_shared<SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 2, 1);
        s->sequence.settings.frameRate = Rational(25);
        s->media[video.id] = video;
        s->generation = 1;
        return s;
    }
    model::Effect effect(std::string_view type) {
        return model::makeEffect(type, video.info, model::SequenceSettings{});
    }
    render::Layer layerAt(const std::shared_ptr<SequenceSnapshot>& s, Rational t, std::size_t index = 0) {
        SequenceVideoSource src(s, false, 0, 0);
        const auto f = src.frameAt(t);
        EXPECT_TRUE(f.has_value() && f->composite);
        return f->composite->layers.at(index);
    }
};

TEST(EffectResolveTest, HelpersParseColorsFlattenPathsAndBuildCurves) {
    const auto green = parseHexColor("#00ff80", {0, 0, 0});
    EXPECT_FLOAT_EQ(green[1], 1.0f);
    EXPECT_NEAR(green[2], 128.0f / 255.0f, 1e-6f);
    EXPECT_FLOAT_EQ(parseHexColor("oops", {0.5f, 0, 0})[0], 0.5f);
    EXPECT_EQ(flattenMaskPath(model::rectanglePath(0, 0, 10, 10), 1, 1).size(), 4u);
    EXPECT_EQ(flattenMaskPath(model::ellipsePath(5, 5, 5, 5), 2, 2).size(), 64u);
    model::Effect e;
    EXPECT_EQ(curveTableOf(e), nullptr);
    e.curves["master"] = {{0, 0}, {0.5, 0.75}, {1, 1}};
    const auto table = curveTableOf(e);
    ASSERT_NE(table, nullptr);
    EXPECT_NEAR(table->rgb[0][128], 0.75f, 0.01f);
}

TEST_F(EffectsSourceTest, LumetriKeyBlurAndMasksReachTheLayer) {
    auto s = snapshot();
    auto c = clip(video, Rational(0), Rational(1));
    auto lumetri = effect(model::kLumetriEffect);
    setParam(lumetri, "shadowsAmount", 50);
    setParam(lumetri, "vignetteAmount", -30);
    lumetri.curves["red"] = {{0, 0.1}, {1, 1}};
    model::Mask mask;
    mask.id = model::MaskId::generate();
    mask.path = model::rectanglePath(0, 0, video.info.width / 2.0, video.info.height);
    lumetri.masks.push_back(mask);
    auto key = effect(model::kUltraKeyEffect);
    key.strings["keyColor"] = "#0000ff";
    auto blur = effect(model::kGaussianBlurEffect);
    setParam(blur, "blurriness", 30);
    c.effects = {lumetri, key, blur};
    s->sequence.videoTracks[0].clips.push_back(c);
    const auto layer = layerAt(s, Rational(5, 25));
    EXPECT_TRUE(layer.grade.lumetri.enabled);
    EXPECT_NE(layer.grade.lumetri.curves, nullptr);
    EXPECT_FLOAT_EQ(layer.grade.lumetri.vignette.amount, -30.0f);
    ASSERT_NE(layer.masks.lumetri, nullptr);
    EXPECT_NEAR(layer.masks.lumetri->sample(0.1f, 0.5f), 1.0f, 0.01f);
    EXPECT_NEAR(layer.masks.lumetri->sample(0.9f, 0.5f), 0.0f, 0.01f);
    EXPECT_TRUE(layer.key.enabled);
    EXPECT_FLOAT_EQ(layer.key.keyColor[2], 1.0f);
    EXPECT_NEAR(layer.blur.sigma, 10.0f * layer.frame->width / layer.sourceWidth, 1e-3);
}

TEST_F(EffectsSourceTest, StabilizerCorrectsWithTheAnalyzedMotion) {
    auto s = snapshot();
    auto c = clip(video, Rational(0), Rational(2));
    auto stabilizer = effect(model::kStabilizerEffect);
    render::StabilizerAnalysis analysis{Rational(25), Rational(0), static_cast<double>(video.info.width),
                                        static_cast<double>(video.info.height), std::vector<render::FrameMotion>(50)};
    for (std::size_t i = 1; i < 50; ++i) {
        analysis.motions[i].dx = i % 2 == 0 ? 6.0 : -6.0; // shaky
    }
    stabilizer.samples = render::encodeStabilizer(analysis);
    c.effects = {stabilizer};
    s->sequence.videoTracks[0].clips.push_back(c);
    const auto layer = layerAt(s, Rational(11, 25));
    EXPECT_NE(layer.pre, render::identityMatrix());
    EXPECT_GT(layer.pre[0], 1.0); // auto-scale zooms in a little
}

TEST_F(EffectsSourceTest, NewTransitionsShapeTheLayers) {
    auto s = snapshot();
    auto a = clip(video, Rational(0), Rational(1));
    auto b = clip(video, Rational(1), Rational(1));
    s->sequence.videoTracks[0].clips = {a, b};
    model::Transition tr;
    tr.id = model::TransitionId::generate();
    tr.from = a.id;
    tr.to = b.id;
    tr.duration = Rational(1, 2);
    const auto at = [&](std::string_view type) {
        tr.type = std::string(type);
        s->sequence.videoTracks[0].transitions = {tr};
        SequenceVideoSource src(s, false, 0, 0);
        return src.frameAt(Rational(1))->composite; // the middle of the transition
    };
    auto push = at(model::kPush);
    ASSERT_EQ(push->layers.size(), 2u);
    EXPECT_NEAR(push->layers[0].transform.positionX, 960.0 - 960.0, 1.0); // outgoing moved half out
    EXPECT_NEAR(push->layers[1].transform.positionX, 960.0 + 960.0, 1.0); // incoming half in
    auto iris = at(model::kIrisRound);
    EXPECT_TRUE(iris->layers[1].wipe.enabled);
    EXPECT_EQ(iris->layers[1].wipe.shape, render::WipeShape::Circle);
    auto white = at(model::kDipToWhite);
    ASSERT_EQ(white->layers.size(), 1u);
    EXPECT_FLOAT_EQ(white->layers[0].grade.dimTarget, 1.0f);
    auto zoom = at(model::kCrossZoom);
    EXPECT_GT(zoom->layers[0].transform.scale, 100.0);
    auto additive = at(model::kAdditiveDissolve);
    EXPECT_EQ(additive->layers[1].transform.blend, model::BlendMode::Add);
}

} // namespace

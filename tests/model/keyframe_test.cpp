#include "model/keyframe.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using loki::Rational;
using loki::model::Interpolation;
using loki::model::Keyframe;
using loki::model::Parameter;

Keyframe key(Rational t, double v, Interpolation i = Interpolation::Linear) {
    Keyframe k;
    k.time = t;
    k.value = v;
    k.interpolation = i;
    return k;
}

TEST(KeyframeTest, StaticParameterIgnoresTime) {
    Parameter p{"scale", 42.0, {}};
    EXPECT_FALSE(p.isAnimated());
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(-5)), 42.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(1000)), 42.0);
}

TEST(KeyframeTest, LinearInterpolation) {
    Parameter p{"x", 0.0, {key(Rational(0), 0.0), key(Rational(2), 100.0)}};
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(0)), 0.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(1)), 50.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(1, 2)), 25.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(2)), 100.0);
}

TEST(KeyframeTest, FlatOutsideKeyframes) {
    Parameter p{"x", 7.0, {key(Rational(1), 10.0), key(Rational(2), 20.0)}};
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(0)), 10.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(5)), 20.0);
}

TEST(KeyframeTest, HoldKeepsValueUntilNextKey) {
    Parameter p{"x", 0.0, {key(Rational(0), 1.0, Interpolation::Hold), key(Rational(1), 2.0)}};
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(999, 1000)), 1.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(1)), 2.0);
}

TEST(KeyframeTest, MultipleSegmentsUseTheirOwnInterpolation) {
    Parameter p{"x", 0.0,
                {key(Rational(0), 0.0, Interpolation::Linear), key(Rational(1), 10.0, Interpolation::Hold),
                 key(Rational(2), 0.0)}};
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(1, 2)), 5.0);
    EXPECT_DOUBLE_EQ(p.valueAt(Rational(3, 2)), 10.0);
}

TEST(KeyframeTest, BezierEaseIsSymmetricMonotonicAndFlatAtEnds) {
    const auto a = key(Rational(0), 0.0, Interpolation::Bezier);
    const auto b = key(Rational(1), 100.0);
    EXPECT_NEAR(loki::model::interpolateSegment(a, b, Rational(1, 2)), 50.0, 1e-9);
    double previous = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double v = loki::model::interpolateSegment(a, b, Rational(i, 100));
        EXPECT_GE(v, previous);
        previous = v;
    }
    // Ease: slower than linear near the ends.
    EXPECT_LT(loki::model::interpolateSegment(a, b, Rational(1, 20)), 5.0);
    EXPECT_GT(loki::model::interpolateSegment(a, b, Rational(19, 20)), 95.0);
    // Symmetry: v(t) + v(1-t) == 100.
    EXPECT_NEAR(loki::model::interpolateSegment(a, b, Rational(1, 5)) +
                    loki::model::interpolateSegment(a, b, Rational(4, 5)),
                100.0, 1e-9);
}

TEST(KeyframeTest, BezierWithZeroInfluenceIsLinear) {
    // With zero influence both handles collapse onto the end points: x(s) == y(s).
    auto a = key(Rational(0), 0.0, Interpolation::Bezier);
    auto b = key(Rational(1), 1.0);
    a.easeOut = 0.0;
    b.easeIn = 0.0;
    for (int i = 0; i <= 10; ++i) {
        EXPECT_NEAR(loki::model::interpolateSegment(a, b, Rational(i, 10)), i / 10.0, 1e-9);
    }
}

TEST(KeyframeTest, Validation) {
    EXPECT_TRUE(loki::model::keyframesValid({}));
    EXPECT_TRUE(loki::model::keyframesValid({key(Rational(0), 1), key(Rational(1), 2)}));
    EXPECT_FALSE(loki::model::keyframesValid({key(Rational(1), 1), key(Rational(0), 2)}));
    EXPECT_FALSE(loki::model::keyframesValid({key(Rational(1), 1), key(Rational(1), 2)}));
    auto bad = key(Rational(0), 1);
    bad.easeIn = 1.5;
    EXPECT_FALSE(loki::model::keyframesValid({bad}));
    auto nan = key(Rational(0), std::nan(""));
    EXPECT_FALSE(loki::model::keyframesValid({nan}));
}

} // namespace

#include "util/rational.h"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {

using loki::Rational;
using loki::Rounding;

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();

TEST(RationalTest, DefaultIsZero) {
    const Rational r;
    EXPECT_EQ(r.num(), 0);
    EXPECT_EQ(r.den(), 1);
    EXPECT_TRUE(r.isZero());
}

TEST(RationalTest, NormalizesSignAndGcd) {
    EXPECT_EQ(Rational(2, 4), Rational(1, 2));
    const Rational neg(1, -2);
    EXPECT_EQ(neg.num(), -1);
    EXPECT_EQ(neg.den(), 2);
    const Rational both(-3, -6);
    EXPECT_EQ(both.num(), 1);
    EXPECT_EQ(both.den(), 2);
    const Rational zero(0, -7);
    EXPECT_EQ(zero.num(), 0);
    EXPECT_EQ(zero.den(), 1);
}

TEST(RationalTest, RejectsZeroDenominatorAndInt64Min) {
    EXPECT_THROW(Rational(1, 0), std::domain_error);
    EXPECT_THROW(Rational(kMin, 1), std::overflow_error);
    EXPECT_THROW(Rational(1, kMin), std::overflow_error);
}

TEST(RationalTest, Arithmetic) {
    EXPECT_EQ(Rational(1, 3) + Rational(1, 6), Rational(1, 2));
    EXPECT_EQ(Rational(1, 3) - Rational(1, 2), Rational(-1, 6));
    EXPECT_EQ(Rational(2, 3) * Rational(9, 4), Rational(3, 2));
    EXPECT_EQ(Rational(2, 3) / Rational(4, 9), Rational(3, 2));
    EXPECT_EQ(-Rational(5, 7), Rational(-5, 7));
    Rational acc(1, 2);
    acc += Rational(1, 4);
    acc *= Rational(4);
    acc -= Rational(1);
    acc /= Rational(2);
    EXPECT_EQ(acc, Rational(1));
}

TEST(RationalTest, NtscFrameMathIsExact) {
    const Rational ntsc(30000, 1001);
    // 30 frames at 29.97 fps last exactly 1.001 s.
    EXPECT_EQ(loki::framesToTime(30, ntsc), Rational(1001, 1000));
    // Summing one frame duration 30000 times gives exactly 1001 s.
    Rational t;
    const Rational frame = ntsc.reciprocal();
    for (int i = 0; i < 30000; ++i) {
        t += frame;
    }
    EXPECT_EQ(t, Rational(1001));
}

TEST(RationalTest, DivisionByZeroThrows) {
    EXPECT_THROW((void)(Rational(1) / Rational(0)), std::domain_error);
    EXPECT_THROW((void)Rational(0).reciprocal(), std::domain_error);
}

TEST(RationalTest, ComparisonIsExactForLargeValues) {
    EXPECT_LT(Rational(1, 3), Rational(1, 2));
    EXPECT_GT(Rational(-1, 3), Rational(-1, 2));
    EXPECT_LT(Rational(kMax - 1, kMax), Rational(1));
    EXPECT_GT(Rational(kMax, kMax - 1), Rational(1));
    // Differ only beyond double precision.
    EXPECT_LT(Rational(kMax - 1, 3), Rational(kMax, 3));
    EXPECT_EQ(Rational(kMax - 1, 3).toDouble(), Rational(kMax, 3).toDouble());
}

TEST(RationalTest, OverflowThrowsInsteadOfWrapping) {
    EXPECT_THROW((void)(Rational(kMax) + Rational(1)), std::overflow_error);
    EXPECT_THROW((void)(Rational(-kMax) - Rational(1)), std::overflow_error);
    EXPECT_THROW((void)(Rational(kMax) * Rational(2)), std::overflow_error);
    EXPECT_THROW((void)(Rational(1, kMax) + Rational(1, kMax - 1)), std::overflow_error);
}

TEST(RationalTest, CrossReductionAvoidsSpuriousOverflow) {
    EXPECT_EQ(Rational(kMax, 3) * Rational(3, kMax), Rational(1));
    EXPECT_EQ(Rational(1, kMax) + Rational(1, kMax), Rational(2, kMax));
}

TEST(RationalTest, FloorCeilRound) {
    EXPECT_EQ(Rational(7, 2).floor(), 3);
    EXPECT_EQ(Rational(7, 2).ceil(), 4);
    EXPECT_EQ(Rational(7, 2).round(), 4);
    EXPECT_EQ(Rational(-7, 2).floor(), -4);
    EXPECT_EQ(Rational(-7, 2).ceil(), -3);
    EXPECT_EQ(Rational(-7, 2).round(), -4);
    EXPECT_EQ(Rational(5, 3).round(), 2);
    EXPECT_EQ(Rational(4, 3).round(), 1);
    EXPECT_EQ(Rational(-5, 3).round(), -2);
    EXPECT_EQ(Rational(6, 3).floor(), 2);
    EXPECT_EQ(Rational(kMax, 2).round(), kMax / 2 + 1);
}

TEST(RationalTest, FrameConversions) {
    const Rational ntsc(30000, 1001);
    EXPECT_EQ(loki::timeToFrames(Rational(1), ntsc, Rounding::Floor), 29);
    EXPECT_EQ(loki::timeToFrames(Rational(1), ntsc, Rounding::Ceil), 30);
    EXPECT_EQ(loki::timeToFrames(Rational(1), ntsc, Rounding::Nearest), 30);
    EXPECT_EQ(loki::quantizeToFrame(Rational(1), Rational(25), Rounding::Nearest), Rational(1));
    EXPECT_EQ(loki::quantizeToFrame(Rational(41, 1000), Rational(25), Rounding::Nearest), Rational(1, 25));
    EXPECT_EQ(loki::quantizeToFrame(Rational(41, 1000), Rational(25), Rounding::Floor), Rational(1, 25));
    EXPECT_EQ(loki::quantizeToFrame(Rational(39, 1000), Rational(25), Rounding::Floor), Rational(0));
    EXPECT_THROW((void)loki::timeToFrames(Rational(1), Rational(0)), std::domain_error);
}

TEST(RationalTest, ToStringAndParseRoundTrip) {
    EXPECT_EQ(Rational(30000, 1001).toString(), "30000/1001");
    EXPECT_EQ(Rational(4, 2).toString(), "2");
    EXPECT_EQ(Rational(-1, 3).toString(), "-1/3");
    for (const auto& r : {Rational(30000, 1001), Rational(-7, 3), Rational(0), Rational(kMax, 7)}) {
        EXPECT_EQ(Rational::parse(r.toString()), r);
    }
    EXPECT_EQ(Rational::parse("+3/4"), Rational(3, 4));
    EXPECT_EQ(Rational::parse("6/-4"), Rational(-3, 2));
}

TEST(RationalTest, ParseRejectsMalformedInput) {
    for (const char* bad : {"", "/", "1/", "/2", "1/0", "a/b", "1/2/3", " 1/2", "1.5", "--1"}) {
        EXPECT_FALSE(Rational::parse(bad).has_value()) << bad;
    }
}

TEST(RationalTest, MinMaxAndHash) {
    EXPECT_EQ(loki::min(Rational(1, 2), Rational(1, 3)), Rational(1, 3));
    EXPECT_EQ(loki::max(Rational(1, 2), Rational(1, 3)), Rational(1, 2));
    std::unordered_set<Rational> set{Rational(1, 2), Rational(2, 4), Rational(3)};
    EXPECT_EQ(set.size(), 2u);
}

} // namespace

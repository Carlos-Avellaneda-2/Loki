#include "util/timecode.h"

#include <gtest/gtest.h>

namespace {

using loki::formatTimecode;
using loki::parseTimecode;
using loki::Rational;

const Rational k2997(30000, 1001);
const Rational k5994(60000, 1001);

TEST(TimecodeTest, NominalFpsAndDropFrameSupport) {
    EXPECT_EQ(loki::nominalFps(k2997), 30);
    EXPECT_EQ(loki::nominalFps(Rational(24000, 1001)), 24);
    EXPECT_EQ(loki::nominalFps(Rational(25)), 25);
    EXPECT_TRUE(loki::supportsDropFrame(k2997));
    EXPECT_TRUE(loki::supportsDropFrame(k5994));
    EXPECT_FALSE(loki::supportsDropFrame(Rational(30)));
    EXPECT_FALSE(loki::supportsDropFrame(Rational(24000, 1001)));
}

TEST(TimecodeTest, FormatsNonDrop) {
    EXPECT_EQ(formatTimecode(0, Rational(25)), "00:00:00:00");
    EXPECT_EQ(formatTimecode(1234, Rational(25)), "00:00:49:09");
    EXPECT_EQ(formatTimecode(90000, Rational(25)), "01:00:00:00");
    EXPECT_EQ(formatTimecode(-30, Rational(30)), "-00:00:01:00");
    EXPECT_EQ(formatTimecode(1800, k2997, false), "00:01:00:00");
}

TEST(TimecodeTest, FormatsDropFrame2997) {
    EXPECT_EQ(formatTimecode(1799, k2997, true), "00:00:59;29");
    EXPECT_EQ(formatTimecode(1800, k2997, true), "00:01:00;02"); // ;00 and ;01 are skipped
    EXPECT_EQ(formatTimecode(17982, k2997, true), "00:10:00;00"); // tenth minute is not skipped
    EXPECT_EQ(formatTimecode(107892, k2997, true), "01:00:00;00");
}

TEST(TimecodeTest, FormatsDropFrame5994) {
    EXPECT_EQ(formatTimecode(3600, k5994, true), "00:01:00;04");
    EXPECT_EQ(formatTimecode(35964, k5994, true), "00:10:00;00");
}

TEST(TimecodeTest, DropFrameFlagIgnoredForUnsupportedRates) {
    EXPECT_EQ(formatTimecode(30, Rational(30), true), "00:00:01:00");
}

TEST(TimecodeTest, ParsesFullAndShortForms) {
    EXPECT_EQ(parseTimecode("00:00:49:09", Rational(25)), 1234);
    EXPECT_EQ(parseTimecode("01:00:00:00", Rational(25)), 90000);
    EXPECT_EQ(parseTimecode("1:00", Rational(30)), 30);   // SS:FF
    EXPECT_EQ(parseTimecode("1:00:00", Rational(30)), 1800); // MM:SS:FF
    EXPECT_EQ(parseTimecode("150", Rational(30)), 150);    // bare frames
    EXPECT_EQ(parseTimecode("-00:00:01:00", Rational(30)), -30);
    EXPECT_EQ(parseTimecode("00:01:00;02", k2997), 1800); // ';' implies drop-frame
    EXPECT_EQ(parseTimecode("00:10:00:00", k2997, true), 17982);
}

TEST(TimecodeTest, RejectsInvalidInput) {
    for (const char* bad : {"", "aa:bb:cc:dd", "00:60:00:00", "00:00:60:00", "00:00:00:30", "1:2:3:4:5", "00::00:00"}) {
        EXPECT_FALSE(parseTimecode(bad, Rational(30)).has_value()) << bad;
    }
    // Labels skipped by drop-frame do not exist.
    EXPECT_FALSE(parseTimecode("00:01:00;00", k2997).has_value());
    EXPECT_FALSE(parseTimecode("00:01:00;01", k2997).has_value());
    EXPECT_TRUE(parseTimecode("00:10:00;00", k2997).has_value());
}

TEST(TimecodeTest, DropFrameRoundTripsEveryFrame) {
    for (std::int64_t f = 0; f < 40000; ++f) {
        const auto tc = formatTimecode(f, k2997, true);
        ASSERT_EQ(parseTimecode(tc, k2997, true), f) << tc;
    }
    for (std::int64_t f = 0; f < 80000; f += 7) {
        const auto tc = formatTimecode(f, k5994, true);
        ASSERT_EQ(parseTimecode(tc, k5994, true), f) << tc;
    }
}

TEST(TimecodeTest, NonDropRoundTrips) {
    for (const auto& rate : {Rational(24), Rational(25), Rational(24000, 1001), Rational(60), Rational(120)}) {
        for (std::int64_t f = -500; f < 200000; f += 37) {
            const auto tc = formatTimecode(f, rate);
            ASSERT_EQ(parseTimecode(tc, rate), f) << tc;
        }
    }
}

} // namespace

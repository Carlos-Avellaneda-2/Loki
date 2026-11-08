#include "util/uuid.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace {

using loki::Uuid;

TEST(UuidTest, DefaultIsNull) {
    EXPECT_TRUE(Uuid().isNull());
    EXPECT_EQ(Uuid().toString(), "00000000-0000-0000-0000-000000000000");
}

TEST(UuidTest, GeneratesUniqueVersion4) {
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 2000; ++i) {
        const auto id = Uuid::generate();
        EXPECT_FALSE(id.isNull());
        EXPECT_EQ(id.bytes()[6] >> 4, 4);           // version
        EXPECT_EQ(id.bytes()[8] & 0xC0, 0x80);       // variant
        EXPECT_TRUE(seen.insert(id).second);
    }
}

TEST(UuidTest, StringRoundTrip) {
    for (int i = 0; i < 100; ++i) {
        const auto id = Uuid::generate();
        const auto text = id.toString();
        EXPECT_EQ(text.size(), 36u);
        EXPECT_EQ(Uuid::parse(text), id);
    }
}

TEST(UuidTest, ParsesUppercaseAndBraces) {
    const auto a = Uuid::parse("{0F1E2D3C-4B5A-4978-8695-A4B3C2D1E0F9}");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->toString(), "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f9");
}

TEST(UuidTest, RejectsMalformed) {
    for (const char* bad : {"", "0f1e2d3c4b5a49788695a4b3c2d1e0f9", "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f",
                            "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0fz", "0f1e2d3c_4b5a-4978-8695-a4b3c2d1e0f9",
                            "{0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f9"}) {
        EXPECT_FALSE(Uuid::parse(bad).has_value()) << bad;
    }
}

TEST(UuidTest, OrderingIsConsistent) {
    const auto a = *Uuid::parse("00000000-0000-4000-8000-000000000001");
    const auto b = *Uuid::parse("00000000-0000-4000-8000-000000000002");
    EXPECT_LT(a, b);
    EXPECT_NE(a, b);
}

} // namespace

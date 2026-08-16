#include <gtest/gtest.h>

#include <unordered_map>

#include "mm/common/InlineString.hpp"

namespace mm {
namespace {

using Str8 = InlineString<8>;

TEST(InlineString, EmptyByDefault) {
    const Str8 s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0U);
    EXPECT_EQ(s.view(), "");
    EXPECT_STREQ(s.c_str(), "");
}

TEST(InlineString, AssignReportsFit) {
    Str8 s;
    EXPECT_TRUE(s.assign("BTCUSDT"));
    EXPECT_EQ(s.view(), "BTCUSDT");
    EXPECT_EQ(s.size(), 7U);
}

TEST(InlineString, AssignReportsTruncation) {
    Str8 s;
    // A truncated identifier is not an identifier. Callers handling venue input
    // must see the failure rather than a plausible-looking prefix.
    EXPECT_FALSE(s.assign("TOOLONGSYMBOL"));
    EXPECT_EQ(s.view(), "TOOLONGS");
    EXPECT_EQ(s.size(), 8U);
}

TEST(InlineString, NulTerminatedForCInterop) {
    Str8 s;
    ASSERT_TRUE(s.assign("ABC"));
    EXPECT_STREQ(s.c_str(), "ABC");
    ASSERT_TRUE(s.assign("12345678"));
    EXPECT_STREQ(s.c_str(), "12345678");
}

TEST(InlineString, AppendRejectsOverflowWithoutMutating) {
    Str8 s;
    ASSERT_TRUE(s.assign("ABCDE"));
    EXPECT_FALSE(s.append("XYZ!"));
    EXPECT_EQ(s.view(), "ABCDE");
    EXPECT_TRUE(s.append("XYZ"));
    EXPECT_EQ(s.view(), "ABCDEXYZ");
}

TEST(InlineString, Clear) {
    Str8 s;
    ASSERT_TRUE(s.assign("ABC"));
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), "");
}

TEST(InlineString, Comparison) {
    EXPECT_EQ(Str8("ABC"), Str8("ABC"));
    EXPECT_NE(Str8("ABC"), Str8("ABD"));
    EXPECT_LT(Str8("ABC"), Str8("ABD"));
    EXPECT_LT(Str8("AB"), Str8("ABC"));
    EXPECT_GT(Str8("B"), Str8("ABC"));
}

TEST(InlineString, UsableAsHashKey) {
    std::unordered_map<Str8, int> m;
    m[Str8("BTCUSDT")] = 1;
    m[Str8("ETHUSDT")] = 2;
    EXPECT_EQ(m.at(Str8("BTCUSDT")), 1);
    EXPECT_EQ(m.at(Str8("ETHUSDT")), 2);
    EXPECT_EQ(m.count(Str8("SOLUSDT")), 0U);
}

TEST(InlineString, IsTriviallyCopyableForRingTransport) {
    // Events carrying identifiers live in ring-buffer slots and are copied
    // byte-wise between threads; anything owning heap memory would break that.
    static_assert(std::is_trivially_copyable_v<Str8>);
    static_assert(std::is_trivially_destructible_v<Str8>);
    SUCCEED();
}

TEST(InlineString, Fnv1aIsConstexpr) {
    constexpr std::uint64_t h = fnv1a("BTCUSDT");
    static_assert(h != 0);
    EXPECT_NE(fnv1a("BTCUSDT"), fnv1a("ETHUSDT"));
}

}  // namespace
}  // namespace mm

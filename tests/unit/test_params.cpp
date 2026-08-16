#include <gtest/gtest.h>

#include "mm/common/Params.hpp"

namespace mm {
namespace {

Params make() {
    Params p;
    p.set("half_spread_bps", "4.5");
    p.set("levels", "3");
    p.set("enabled", "true");
    p.set("mode", "aggressive");
    p.set("tick", "0.01");
    p.set("size", "0.001");
    return p;
}

TEST(Params, TypedGetters) {
    const Params p = make();
    EXPECT_DOUBLE_EQ(p.get_double("half_spread_bps").value(), 4.5);
    EXPECT_EQ(p.get_int("levels").value(), 3);
    EXPECT_TRUE(p.get_bool("enabled").value());
    EXPECT_EQ(p.get_string("mode").value(), "aggressive");
    EXPECT_EQ(p.get_px("tick").value().to_string(), "0.01");
    EXPECT_EQ(p.get_qty("size").value().to_string(), "0.001");
}

TEST(Params, MissingRequiredParamIsAnError) {
    // A mistyped parameter name must stop the strategy from constructing, not
    // silently trade with a default the researcher never chose.
    const Params p = make();
    const auto r = p.get_double("half_sprad_bps");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::NotFound);
    EXPECT_NE(r.status().message().find("half_sprad_bps"), std::string_view::npos);
}

TEST(Params, WrongTypeIsAnError) {
    Params p;
    p.set("levels", "three");
    const auto r = p.get_int("levels");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.status().code(), ErrorCode::ParseError);
}

TEST(Params, PartialNumberIsRejected) {
    Params p;
    p.set("n", "12abc");
    EXPECT_TRUE(p.get_int("n").is_error());
    p.set("d", "1.5x");
    EXPECT_TRUE(p.get_double("d").is_error());
}

TEST(Params, BooleanSpellings) {
    Params p;
    for (const char* t : {"true", "True", "TRUE", "yes", "on", "1"}) {
        p.set("v", t);
        EXPECT_TRUE(p.get_bool("v").value()) << t;
    }
    for (const char* f : {"false", "False", "FALSE", "no", "off", "0"}) {
        p.set("v", f);
        EXPECT_FALSE(p.get_bool("v").value()) << f;
    }
    p.set("v", "maybe");
    EXPECT_TRUE(p.get_bool("v").is_error());
}

TEST(Params, DefaultedGetters) {
    const Params p = make();
    EXPECT_DOUBLE_EQ(p.double_or("absent", 1.25), 1.25);
    EXPECT_EQ(p.int_or("absent", 7), 7);
    EXPECT_TRUE(p.bool_or("absent", true));
    EXPECT_EQ(p.string_or("absent", "fallback"), "fallback");
    // A present-but-invalid value also falls back rather than throwing.
    Params bad;
    bad.set("x", "not-a-number");
    EXPECT_DOUBLE_EQ(bad.double_or("x", 9.0), 9.0);
}

TEST(Params, KeysAreSortedForStableAudit) {
    const Params p = make();
    const auto keys = p.keys();
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys.size(), 6U);
}

TEST(Params, ToStringIsDeterministic) {
    Params a, b;
    a.set("z", "1");
    a.set("a", "2");
    b.set("a", "2");
    b.set("z", "1");
    EXPECT_EQ(a.to_string(), b.to_string()) << "the audit record must not depend on insertion order";
    EXPECT_EQ(a.to_string(), "{a=2, z=1}");
}

}  // namespace
}  // namespace mm

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "mm/common/Metrics.hpp"

namespace mm {
namespace {

TEST(Counter, Increments) {
    Counter c;
    EXPECT_EQ(c.value(), 0U);
    c.inc();
    c.inc(5);
    EXPECT_EQ(c.value(), 6U);
    c.reset();
    EXPECT_EQ(c.value(), 0U);
}

TEST(Gauge, SetsAndAdjusts) {
    Gauge g;
    g.set(10);
    g.inc();
    g.dec();
    g.add(-4);
    EXPECT_EQ(g.value(), 6);
}

TEST(Counter, IsAtomicUnderContention) {
    Counter c;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&c] {
            for (int n = 0; n < 50'000; ++n) {
                c.inc();
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(c.value(), 400'000U);
}

TEST(MetricsRegistry, ReturnsStableReferences) {
    MetricsRegistry reg;
    Counter& a = reg.counter("orders_submitted");
    a.inc(3);
    // Hot-path code caches the reference at startup; a later lookup must resolve
    // to the same object or the instrumentation silently splits in two.
    Counter& b = reg.counter("orders_submitted");
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(b.value(), 3U);
}

TEST(MetricsRegistry, LabelOrderDoesNotCreateDuplicates) {
    MetricsRegistry reg;
    Counter& a = reg.counter("fills", {{"symbol", "BTCUSDT"}, {"side", "BUY"}});
    Counter& b = reg.counter("fills", {{"side", "BUY"}, {"symbol", "BTCUSDT"}});
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(reg.size(), 1U);
}

TEST(MetricsRegistry, DifferentLabelsAreDifferentSeries) {
    MetricsRegistry reg;
    reg.counter("fills", {{"symbol", "BTCUSDT"}}).inc();
    reg.counter("fills", {{"symbol", "ETHUSDT"}}).inc(2);
    EXPECT_EQ(reg.size(), 2U);
}

TEST(MetricsRegistry, PrometheusExposition) {
    MetricsRegistry reg;
    reg.describe("orders_submitted", "Orders sent to the venue");
    reg.counter("orders_submitted", {{"symbol", "BTCUSDT"}}).inc(7);
    reg.gauge("position", {{"symbol", "BTCUSDT"}}).set(-3);

    const std::string text = reg.render_prometheus();
    EXPECT_NE(text.find("# HELP orders_submitted Orders sent to the venue"), std::string::npos);
    EXPECT_NE(text.find("# TYPE orders_submitted counter"), std::string::npos);
    EXPECT_NE(text.find("orders_submitted{symbol=\"BTCUSDT\"} 7"), std::string::npos);
    EXPECT_NE(text.find("# TYPE position gauge"), std::string::npos);
    EXPECT_NE(text.find("position{symbol=\"BTCUSDT\"} -3"), std::string::npos);
}

TEST(MetricsRegistry, EscapesLabelValues) {
    MetricsRegistry reg;
    reg.counter("errors", {{"reason", "bad \"quote\""}}).inc();
    const std::string text = reg.render_prometheus();
    EXPECT_NE(text.find("reason=\"bad \\\"quote\\\"\""), std::string::npos);
}

TEST(MetricsRegistry, HelpEmittedOncePerFamily) {
    MetricsRegistry reg;
    reg.describe("fills", "Fill count");
    reg.counter("fills", {{"symbol", "BTCUSDT"}}).inc();
    reg.counter("fills", {{"symbol", "ETHUSDT"}}).inc();

    const std::string text = reg.render_prometheus();
    std::size_t occurrences = 0;
    for (std::size_t pos = text.find("# HELP fills"); pos != std::string::npos;
         pos = text.find("# HELP fills", pos + 1)) {
        ++occurrences;
    }
    EXPECT_EQ(occurrences, 1U);
}

TEST(MetricsRegistry, ResetAll) {
    MetricsRegistry reg;
    reg.counter("a").inc(5);
    reg.gauge("b").set(9);
    reg.reset_all();
    EXPECT_EQ(reg.counter("a").value(), 0U);
    EXPECT_EQ(reg.gauge("b").value(), 0);
}

}  // namespace
}  // namespace mm

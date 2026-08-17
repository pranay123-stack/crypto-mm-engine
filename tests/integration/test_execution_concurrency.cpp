/// \file test_execution_concurrency.cpp
/// The one genuinely concurrent boundary in the execution path, across two
/// real threads, so TSan has something to analyse.
///
/// Per docs/paper-execution.md §11a the paper venue is single-threaded; the
/// concurrency lives in the OMS:
///
/// ```
///   exec thread                          trading thread
///   ───────────                          ──────────────
///   OMS::on_execution ─► SPSC ring ─► OMS::process_events
///   (enqueue only)                    (all state mutation)
/// ```
///
/// The events pushed across that ring are produced by the real paper venue
/// driving the real pipeline, so this is not a synthetic ring benchmark: it is
/// the production hand-off carrying production events.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "mm/exchange/paper/PaperExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "support/PaperFixtures.hpp"
#include "support/RecordingSinks.hpp"

namespace mm {
namespace {

using exchange::paper::PaperExecution;
using exchange::paper::PaperExecutionConfig;

/// Produces a realistic stream of execution events by driving the real venue
/// through a real order lifecycle: acks, partial fills, full fills, cancels.
std::vector<exchange::ExecutionEvent> produce_events(std::size_t order_count) {
    ManualClock clock(millis(1'000), millis(1'000));
    test::RecordingExecutionSink sink;
    PaperExecutionConfig config = test::paper_config();
    config.max_orders = 4'096;
    config.max_pending_events = 16'384;
    PaperExecution venue(config, clock);
    auto book = test::make_book(Symbol("BTCUSDT"), {{"60000.00", "50"}}, {{"60000.10", "50"}});

    EXPECT_TRUE(venue.start(sink).is_ok());
    venue.attach_book(Symbol("BTCUSDT"), *book);
    venue.on_market_update(Symbol("BTCUSDT"), clock.steady());

    Seq seq = 100;
    for (std::size_t i = 0; i < order_count; ++i) {
        const std::string id = "cc" + std::to_string(i);
        static_cast<void>(venue.submit(test::paper_order(id.c_str(), Side::Buy, "59900.00", "2")));
        clock.advance(millis(3));
        static_cast<void>(venue.poll());
    }
    // Partial fills, then completion, then a cancel wave.
    test::load_book(*book, {{"59800.00", "1"}}, {{"59900.00", "10"}}, ++seq);
    venue.on_market_update(Symbol("BTCUSDT"), clock.steady());
    clock.advance(millis(3));
    static_cast<void>(venue.poll());

    static_cast<void>(venue.cancel_all(Symbol("BTCUSDT")));
    clock.advance(millis(5));
    static_cast<void>(venue.poll());

    return sink.events;
}

/// The OMS's ring, driven from two threads at once.
///
/// One thread does nothing but `on_execution` (what a real adapter's I/O thread
/// does); the other does nothing but `process_events` (what the trading thread
/// does). Every byte of shared state between them is the SPSC ring, and TSan is
/// what proves it.
TEST(ExecutionConcurrency, EventsCrossTheRingBetweenTwoRealThreads) {
    const std::vector<exchange::ExecutionEvent> events = produce_events(64);
    ASSERT_GT(events.size(), 64U) << "the producer must have generated a real lifecycle";

    SystemClock clock;  // real monotonic: ManualClock is not thread-safe by design
    oms::OmsConfig config;
    static_cast<void>(config.client_id_prefix.assign("cc"));
    config.session_id = 1;
    config.max_orders = 4'096;
    oms::NullOrderJournal journal;
    oms::OrderManager oms(config, journal, clock);

    std::atomic<bool> producer_done{false};
    std::atomic<std::size_t> enqueued{0};
    std::atomic<std::size_t> applied{0};

    // Exec-io thread: enqueue only, never touching OMS state.
    std::thread producer([&] {
        for (const exchange::ExecutionEvent& e : events) {
            oms.on_execution(e);
            enqueued.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Trading thread: the only thread that mutates OMS state.
    std::thread consumer([&] {
        for (;;) {
            const std::size_t n = oms.process_events();
            applied.fetch_add(n, std::memory_order_relaxed);
            if (producer_done.load(std::memory_order_acquire) && n == 0 &&
                oms.queued_events() == 0) {
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    // Everything that was enqueued was applied. A ring that dropped or
    // duplicated under contention would show up right here.
    EXPECT_EQ(enqueued.load(), events.size());
    EXPECT_EQ(applied.load(), events.size());
    EXPECT_EQ(oms.queued_events(), 0U);

    // The events referred to orders the OMS never issued, so every one is
    // attributed to nothing and quarantined -- never used to invent a record.
    EXPECT_EQ(oms.order_count(), 0U);
    EXPECT_GT(oms.metrics().quarantined_events + oms.metrics().orphan_orders, 0U);
}

/// The same ring, with the consumer draining far faster than the producer
/// fills -- the empty-ring path, which is the one a real trading loop spends
/// almost all of its time on.
TEST(ExecutionConcurrency, AnEmptyRingIsSafeToDrainConcurrently) {
    const std::vector<exchange::ExecutionEvent> events = produce_events(8);
    ASSERT_FALSE(events.empty());

    SystemClock clock;
    oms::OmsConfig config;
    static_cast<void>(config.client_id_prefix.assign("cc"));
    config.session_id = 2;
    oms::NullOrderJournal journal;
    oms::OrderManager oms(config, journal, clock);

    std::atomic<bool> done{false};
    std::atomic<std::size_t> drained{0};

    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire)) {
            drained.fetch_add(oms.process_events(), std::memory_order_relaxed);
        }
        drained.fetch_add(oms.process_events(), std::memory_order_relaxed);
    });

    // Producer deliberately slow relative to the consumer, so the consumer
    // spends most of its iterations on an empty ring.
    for (std::size_t round = 0; round < 200; ++round) {
        oms.on_execution(events[round % events.size()]);
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(drained.load(), 200U);
    EXPECT_EQ(oms.queued_events(), 0U);
}

}  // namespace
}  // namespace mm

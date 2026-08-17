/// OMS benchmarks (Phase 8 §43).
///
/// The OMS sits between risk and the execution adapter, and again between the
/// adapter and everything that reads working state. Both directions are on the
/// trading thread, so both are measured.
///
/// Phase 2 discipline applies: a sub-nanosecond result for real work means the
/// benchmark is broken, not that the code is fast. Every fixture below varies
/// its input so nothing can be hoisted out of the timed region, and anything
/// built per-iteration is built outside it.

#include <benchmark/benchmark.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "mm/exchange/mock/MockExchangeExecution.hpp"
#include "mm/oms/OrderManager.hpp"
#include "mm/risk/RiskEngine.hpp"

namespace mm::oms {
namespace {

constexpr const char* kOwner = "bench_strategy_v1";

InstrumentSpec instrument() {
    InstrumentSpec s;
    s.symbol = Symbol("BTCUSDT");
    static_cast<void>(Px::parse("0.01", s.tick_size));
    static_cast<void>(Qty::parse("0.001", s.lot_size));
    static_cast<void>(Qty::parse("0.001", s.min_qty));
    static_cast<void>(Qty::parse("1000", s.max_qty));
    static_cast<void>(Notional::parse("5", s.min_notional));
    s.status = MarketStatus::Trading;
    return s;
}

risk::RiskLimits limits() {
    risk::SymbolLimits btc;
    btc.symbol = Symbol("BTCUSDT");
    static_cast<void>(Qty::parse("100000", btc.max_position));
    static_cast<void>(Notional::parse("100000000000", btc.max_position_notional));
    static_cast<void>(Qty::parse("1000", btc.max_order_quantity));
    static_cast<void>(Notional::parse("100000000", btc.max_order_notional));
    static_cast<void>(Qty::parse("100000", btc.max_working_exposure));
    static_cast<void>(Qty::parse("100000", btc.max_side_exposure));
    btc.max_open_orders = 100'000;
    btc.price_band_bps = 10'000;

    risk::RiskLimits l;
    l.global.max_market_data_age_ns = seconds(3'600);
    l.global.max_position_age_ns = seconds(3'600);
    static_cast<void>(l.add(btc));
    return l;
}

OmsConfig config(std::size_t max_orders = 4'096) {
    OmsConfig c;
    static_cast<void>(c.client_id_prefix.assign("mm"));
    c.session_id = 1;
    c.max_orders = max_orders;
    c.new_request_timeout_ns = seconds(3'600);
    c.cancel_request_timeout_ns = seconds(3'600);
    c.replace_request_timeout_ns = seconds(3'600);
    return c;
}

quote::OrderAction new_action(Side side, Px price, Qty quantity, std::uint64_t generation) {
    quote::OrderAction a;
    a.type = quote::OrderActionType::New;
    a.slot = quote::slot_of(side);
    a.side = side;
    a.symbol = Symbol("BTCUSDT");
    a.price = price;
    a.quantity = quantity;
    static_cast<void>(a.identity.name.assign(kOwner));
    a.identity.version = 1;
    a.generation = generation;
    return a;
}

/// An execution adapter that does nothing at all.
///
/// The mock adapter models venue *behaviour*, which means map inserts and event
/// scheduling. Including that in a benchmark labelled "OMS submit" would report
/// the mock's cost as the OMS's. This one exists so the submit path measures
/// only the OMS: identity, state, journal, and the virtual call out.
class NullExecution final : public exchange::IExchangeExecution {
public:
    [[nodiscard]] Status start(exchange::IExecutionSink&) override { return Status::ok(); }
    void stop() override {}
    [[nodiscard]] Status submit(const exchange::OrderRequest&) override {
        ++requests;
        return Status::ok();
    }
    [[nodiscard]] Status cancel(const exchange::CancelRequest&) override {
        ++requests;
        return Status::ok();
    }
    [[nodiscard]] Status replace(const exchange::ReplaceRequest&) override {
        ++requests;
        return Status::ok();
    }
    [[nodiscard]] Status cancel_all(const Symbol&) override { return Status::ok(); }
    [[nodiscard]] Status query_open_orders(const Symbol&) override { return Status::ok(); }
    [[nodiscard]] Status query_order(const ClientOrderId&, const ExchangeOrderId&) override {
        return Status::ok();
    }
    [[nodiscard]] Status query_balances() override { return Status::ok(); }
    [[nodiscard]] Status query_positions() override { return Status::ok(); }
    [[nodiscard]] exchange::ConnectionState connection_state() const override {
        return exchange::ConnectionState::Connected;
    }
    [[nodiscard]] bool is_authenticated() const override { return true; }
    [[nodiscard]] std::size_t in_flight_requests() const override { return 0; }
    [[nodiscard]] const exchange::ExchangeCapabilities& capabilities() const override {
        return caps;
    }
    [[nodiscard]] VenueName venue() const override { return VenueName("bench"); }

    exchange::ExchangeCapabilities caps = exchange::mock::permissive_mock_capabilities();
    std::uint64_t requests = 0;
};

/// Everything a benchmark needs to submit orders, wired the way production is.
struct Harness {
    explicit Harness(std::size_t max_orders = 4'096)
        : clock(millis(1'000), 0),
          venue(clock),
          risk(limits(), clock),
          oms(config(max_orders), journal, clock) {
        spec = instrument();
        static_cast<void>(risk.arm());
        static_cast<void>(venue.start(oms));
        oms.attach_execution(venue);
        static_cast<void>(oms.process_events());
    }

    [[nodiscard]] risk::ApprovedAction approve(const quote::OrderAction& action) {
        risk::RiskInput in;
        in.symbol = Symbol("BTCUSDT");
        in.instrument = &spec;
        in.position.valid = true;
        in.position.symbol = Symbol("BTCUSDT");
        in.position.sequence = 1;
        in.exposure.determinate = true;
        static_cast<void>(Px::parse("60000.00", in.bbo.bid_px));
        static_cast<void>(Qty::parse("100", in.bbo.bid_qty));
        static_cast<void>(Px::parse("60000.10", in.bbo.ask_px));
        static_cast<void>(Qty::parse("100", in.bbo.ask_qty));
        in.system_ready = true;
        // Risk checks the owner it was told to expect. Every action here is
        // its own owner, which is how each one lands on its own slot.
        in.expected_owner = action.identity.name;
        return risk.evaluate(action, in).approval;
    }

    [[nodiscard]] SubmitResult submit_for_bench(const risk::ApprovedAction& approved) {
        return oms.submit(approved);
    }

    /// Fills the order table with acknowledged working orders, which is the
    /// state a lookup or a working-state publication actually runs against.
    void populate(std::size_t count) {
        Qty qty;
        static_cast<void>(Qty::parse("0.001", qty));
        for (std::size_t i = 0; i < count; ++i) {
            Px px;
            static_cast<void>(Px::parse(
                (std::to_string(50'000 + (i % 5'000)) + ".00").c_str(), px));
            const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            quote::OrderAction a = new_action(side, px, qty, i + 1);
            // Distinct slots so each order gets its own record rather than
            // colliding with the previous one.
            a.slot = static_cast<quote::QuoteSlot>(i % 2);
            static_cast<void>(a.identity.name.assign(
                (std::string(kOwner) + std::to_string(i)).c_str()));
            const risk::ApprovedAction approved = approve(a);
            if (!approved.is_valid() || !oms.submit(approved).accepted) {
                continue;
            }
            // Past the mock's acknowledgement latency, so these orders end up
            // WORKING. Leaving them PENDING_NEW would silently turn every
            // benchmark below into a measurement of the wrong state.
            clock.advance(millis(1));
            static_cast<void>(venue.pump());
            static_cast<void>(oms.process_events());
        }
    }

    ManualClock clock;
    InstrumentSpec spec{};
    exchange::mock::MockExchangeExecution venue;
    risk::RiskEngine risk;
    NullOrderJournal journal;
    OrderManager oms;
};

// ---------------------------------------------------------------------------
// Order creation -- risk approval in hand, through to the request being sent
// ---------------------------------------------------------------------------

/// Submit cost against a resident order book of `range(0)` orders.
///
/// The parameter matters: locating the slot an action targets is a linear scan
/// over resident orders, so submit is O(resident). A two-sided quoter on a
/// handful of symbols lives at the low end; the larger arguments are here to
/// make the scaling visible rather than to hide it behind an average.
void BM_OmsSubmitNew(benchmark::State& state) {
    const auto resident = static_cast<std::size_t>(state.range(0));
    // Small, because every submit adds a record: a large batch would let the
    // resident set drift far above `resident` and report the average of a
    // growing table as the cost of a submit.
    constexpr std::size_t kBatch = 16;

    auto h = std::make_unique<Harness>(4'096);
    h->populate(resident);
    // Swap the venue out from under the OMS so the timed region contains no
    // adapter work at all.
    NullExecution null_venue;
    h->oms.attach_execution(null_venue);

    Qty qty;
    static_cast<void>(Qty::parse("0.001", qty));

    // Approvals are minted outside the timed region: this measures the OMS,
    // not the risk engine, which has its own benchmarks. Each carries a
    // distinct owner so every submit creates a record instead of colliding
    // with the previous one on the same slot.
    std::vector<risk::ApprovedAction> approvals;
    approvals.reserve(kBatch);
    for (std::size_t i = 0; i < kBatch; ++i) {
        Px px;
        static_cast<void>(Px::parse((std::to_string(50'000 + (i % 5'000)) + ".00").c_str(), px));
        quote::OrderAction a = new_action(i % 2 == 0 ? Side::Buy : Side::Sell, px, qty, i + 1);
        static_cast<void>(a.identity.name.assign(
            ("bench_submit_" + std::to_string(i)).c_str()));
        approvals.push_back(h->approve(a));
    }

    std::size_t i = 0;
    for (auto _ : state) {
        SubmitResult r = h->submit_for_bench(approvals[i % kBatch]);
        benchmark::DoNotOptimize(r);
        ++i;
        if (i % kBatch == 0) {
            // Rebuilding is not part of the measurement. Without it the
            // resident set would grow without bound and this would become a
            // benchmark of table growth rather than of submit.
            state.PauseTiming();
            h = std::make_unique<Harness>(4'096);
            h->populate(resident);
            h->oms.attach_execution(null_venue);
            for (std::size_t j = 0; j < kBatch; ++j) {
                Px px;
                static_cast<void>(
                    Px::parse((std::to_string(50'000 + (j % 5'000)) + ".00").c_str(), px));
                quote::OrderAction a =
                    new_action(j % 2 == 0 ? Side::Buy : Side::Sell, px, qty, j + 1);
                static_cast<void>(a.identity.name.assign(
                    ("bench_submit_" + std::to_string(j)).c_str()));
                approvals[j] = h->approve(a);
            }
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsSubmitNew)->Arg(4)->Arg(32)->Arg(256);

// ---------------------------------------------------------------------------
// State transition -- the table lookup plus the journal write
// ---------------------------------------------------------------------------

void BM_OmsLegalTransitionCheck(benchmark::State& state) {
    constexpr std::array<OrderState, 6> kStates{OrderState::PendingNew, OrderState::Working,
                                                OrderState::PartiallyFilled,
                                                OrderState::PendingCancel,
                                                OrderState::PendingReplace, OrderState::Unknown};
    std::size_t i = 0;
    for (auto _ : state) {
        // Both operands vary, so nothing folds to a constant.
        bool legal = is_legal_transition(kStates[i % kStates.size()],
                                         kStates[(i + 3) % kStates.size()]);
        benchmark::DoNotOptimize(legal);
        ++i;
    }
}
BENCHMARK(BM_OmsLegalTransitionCheck);

// ---------------------------------------------------------------------------
// Event processing -- an acknowledgement crossing the ring and being applied
// ---------------------------------------------------------------------------

void BM_OmsProcessAcknowledgement(benchmark::State& state) {
    constexpr std::size_t kBatch = 2'048;
    auto h = std::make_unique<Harness>(200'000);
    Qty qty;
    Px px;
    static_cast<void>(Qty::parse("0.001", qty));
    static_cast<void>(Px::parse("60000.00", px));

    std::size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (i % kBatch == 0 && i != 0) {
            h = std::make_unique<Harness>(200'000);
        }
        quote::OrderAction a = new_action(Side::Buy, px, qty, i + 1);
        static_cast<void>(a.identity.name.assign(
            (std::string(kOwner) + std::to_string(i % kBatch)).c_str()));
        const risk::ApprovedAction approved = h->approve(a);
        const bool placed = approved.is_valid() && h->oms.submit(approved).accepted;
        if (placed) {
            h->clock.advance(millis(1));
            // Enqueues the acknowledgement into the ring; does not apply it.
            static_cast<void>(h->venue.pump());
        }
        ++i;
        state.ResumeTiming();

        std::size_t applied = h->oms.process_events();
        benchmark::DoNotOptimize(applied);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsProcessAcknowledgement);

// ---------------------------------------------------------------------------
// Duplicate detection -- the trade-id ring an out-of-order venue exercises
// ---------------------------------------------------------------------------

void BM_OmsDuplicateTradeIdLookup(benchmark::State& state) {
    SeenTradeIds seen;
    for (int i = 0; i < 24; ++i) {
        static_cast<void>(seen.insert(TradeId(("T" + std::to_string(i)).c_str())));
    }
    // A mix of hits and misses: a benchmark that only ever misses measures the
    // cheap path and reports it as the cost.
    std::vector<TradeId> probes;
    probes.reserve(64);
    for (int i = 0; i < 64; ++i) {
        probes.emplace_back(("T" + std::to_string(i)).c_str());
    }

    std::size_t i = 0;
    for (auto _ : state) {
        bool hit = seen.contains(probes[i % probes.size()]);
        benchmark::DoNotOptimize(hit);
        benchmark::DoNotOptimize(seen);
        ++i;
    }
}
BENCHMARK(BM_OmsDuplicateTradeIdLookup);

// ---------------------------------------------------------------------------
// Fill processing -- lookup, idempotency, VWAP, state transition
// ---------------------------------------------------------------------------

void BM_OmsProcessFill(benchmark::State& state) {
    Harness h(4'096);
    h.populate(64);

    // A large original quantity so a long run of small fills never completes
    // the order and starts measuring the terminal path instead.
    Px px;
    Qty qty;
    static_cast<void>(Px::parse("60000.00", px));
    static_cast<void>(Qty::parse("1000", qty));
    quote::OrderAction a = new_action(Side::Buy, px, qty, 99'999);
    static_cast<void>(a.identity.name.assign("bench_fill_owner"));
    const risk::ApprovedAction approved = h.approve(a);
    if (!approved.is_valid() || !h.oms.submit(approved).accepted) {
        state.SkipWithError("fixture failed to place the order under benchmark");
        return;
    }
    h.clock.advance(millis(1));
    static_cast<void>(h.venue.pump());
    static_cast<void>(h.oms.process_events());

    ClientOrderId target;
    for (std::uint64_t raw = 1; raw <= h.oms.order_count(); ++raw) {
        const OrderRecord* o = h.oms.find(LogicalOrderId{raw});
        if (o != nullptr && o->ownership.strategy == StrategyName("bench_fill_owner")) {
            target = o->client_order_id;
        }
    }
    if (target.empty()) {
        state.SkipWithError("fixture could not identify the order under benchmark");
        return;
    }

    Qty slice;
    static_cast<void>(Qty::parse("0.001", slice));
    std::uint64_t trade = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // A fresh trade id every iteration, so this measures the accepted-fill
        // path rather than the duplicate rejection.
        char id[24];
        std::snprintf(id, sizeof(id), "B%llu", static_cast<unsigned long long>(++trade));
        static_cast<void>(h.venue.deliver_fill(target, px, slice, Liquidity::Maker, TradeId(id)));
        state.ResumeTiming();

        std::size_t applied = h.oms.process_events();
        benchmark::DoNotOptimize(applied);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsProcessFill);

// ---------------------------------------------------------------------------
// Reconciliation lookup -- finding a local order from a venue row
// ---------------------------------------------------------------------------

void BM_OmsLookupByClientId(benchmark::State& state) {
    Harness h(4'096);
    const auto count = static_cast<std::size_t>(state.range(0));
    h.populate(count);

    std::vector<ClientOrderId> ids;
    ids.reserve(h.oms.order_count());
    for (std::uint64_t raw = 1; raw <= h.oms.order_count(); ++raw) {
        const OrderRecord* o = h.oms.find(LogicalOrderId{raw});
        if (o != nullptr && !o->client_order_id.empty()) {
            ids.push_back(o->client_order_id);
        }
    }
    if (ids.empty()) {
        state.SkipWithError("fixture produced no orders to look up");
        return;
    }

    std::size_t i = 0;
    for (auto _ : state) {
        const OrderRecord* found = h.oms.find(ids[i % ids.size()]);
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(found);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsLookupByClientId)->Arg(16)->Arg(256)->Arg(1024);

void BM_OmsReconcile(benchmark::State& state) {
    Harness h(4'096);
    const auto count = static_cast<std::size_t>(state.range(0));
    h.populate(count);

    // A snapshot that agrees with local state: the common case, and the one
    // that does the most comparison work per row.
    VenueSnapshot snapshot;
    snapshot.is_complete = true;
    snapshot.symbol_filter = Symbol("BTCUSDT");
    snapshot.received_ns = h.clock.steady();
    for (std::uint64_t raw = 1; raw <= h.oms.order_count(); ++raw) {
        const OrderRecord* o = h.oms.find(LogicalOrderId{raw});
        if (o == nullptr || !o->is_live()) {
            continue;
        }
        exchange::OrderStatusReport row;
        row.client_order_id = o->client_order_id;
        row.exchange_order_id = o->exchange_order_id;
        row.symbol = o->symbol;
        row.side = o->side;
        row.status = exchange::OrderStatus::New;
        row.price = o->price;
        row.original_qty = o->original_quantity;
        row.cumulative_qty = o->cumulative_quantity;
        snapshot.orders.push_back(row);
    }

    if (snapshot.orders.empty()) {
        // An empty snapshot would measure the missing-order path and report it
        // as the cost of reconciliation.
        state.SkipWithError("fixture produced an empty venue snapshot");
        return;
    }

    for (auto _ : state) {
        ReconciliationReport report = h.oms.reconcile(snapshot);
        benchmark::DoNotOptimize(report.orders_compared);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(snapshot.orders.size()));
}
BENCHMARK(BM_OmsReconcile)->Arg(16)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Working-state publication -- what the quote manager reads every cycle
// ---------------------------------------------------------------------------

void BM_OmsWorkingState(benchmark::State& state) {
    Harness h(4'096);
    h.populate(static_cast<std::size_t>(state.range(0)));

    if (h.oms.live_order_count() == 0) {
        state.SkipWithError("fixture produced no working orders");
        return;
    }
    // The owner that actually holds slots, so this measures the matching path
    // rather than a scan that rejects everything immediately.
    const StrategyName owner((std::string(kOwner) + "0").c_str());
    const StrategyName other((std::string(kOwner) + "1").c_str());
    std::size_t i = 0;
    for (auto _ : state) {
        quote::WorkingQuoteState published =
            h.oms.working_state(Symbol("BTCUSDT"), (i % 2 == 0) ? owner : other);
        benchmark::DoNotOptimize(published);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsWorkingState)->Arg(16)->Arg(256)->Arg(1024);

void BM_OmsExposure(benchmark::State& state) {
    Harness h(4'096);
    h.populate(static_cast<std::size_t>(state.range(0)));
    if (h.oms.live_order_count() == 0) {
        state.SkipWithError("fixture produced no working orders");
        return;
    }

    for (auto _ : state) {
        risk::ExposureSnapshot exposure = h.oms.exposure(Symbol("BTCUSDT"));
        benchmark::DoNotOptimize(exposure);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OmsExposure)->Arg(16)->Arg(256)->Arg(1024);

}  // namespace
}  // namespace mm::oms

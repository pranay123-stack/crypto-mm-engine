#include "ReferenceMarketMaker.hpp"

#include "mm/strategy/StrategyRegistry.hpp"

namespace mm::strategies {
namespace {

using strategy::IntentReason;
using strategy::QuoteAction;
using strategy::QuoteIntent;
using strategy::StrategyContext;

constexpr const char* kName = "reference_mm_v1";
constexpr std::int32_t kVersion = 1;

/// Shifts a price by a basis-point offset, staying in fixed point. Going via a
/// double here would reintroduce exactly the rounding the fixed-point types
/// exist to remove.
[[nodiscard]] Px shift_bps(Px price, double bps) noexcept {
    const auto scaled = static_cast<std::int64_t>(bps * 100.0);  // bps -> 1e-6 units
    return price.scaled_by(1'000'000 + scaled, 1'000'000);
}

}  // namespace

Status ReferenceMarketMaker::initialize(const strategy::StrategyInit& init) {
    // Every parameter is read explicitly and a bad one is an error, not a
    // default. A mistyped parameter name must stop the engine from starting
    // rather than let it quote a spread nobody chose.
    const Params& p = init.params;

    const auto half_spread = p.get_double("half_spread_bps");
    if (half_spread.is_error()) {
        return half_spread.status();
    }
    if (half_spread.value() <= 0.0) {
        return {ErrorCode::InvalidArgument, "half_spread_bps must be positive"};
    }
    params_.half_spread_bps = half_spread.value();

    const auto size = p.get_qty("quote_size");
    if (size.is_error()) {
        return size.status();
    }
    if (!size.value().is_positive()) {
        return {ErrorCode::InvalidArgument, "quote_size must be positive"};
    }
    params_.quote_size = size.value();

    params_.inventory_skew_bps = p.double_or("inventory_skew_bps", 0.0);
    if (params_.inventory_skew_bps < 0.0) {
        return {ErrorCode::InvalidArgument, "inventory_skew_bps must not be negative"};
    }
    params_.max_inventory_utilisation = p.double_or("max_inventory_utilisation", 0.9);
    if (params_.max_inventory_utilisation <= 0.0 || params_.max_inventory_utilisation > 1.0) {
        return {ErrorCode::InvalidArgument, "max_inventory_utilisation must be in (0, 1]"};
    }
    params_.min_market_spread_bps = p.double_or("min_market_spread_bps", 0.0);
    params_.use_micro_price = p.bool_or("use_micro_price", false);

    if (!init.instrument.is_valid()) {
        return {ErrorCode::FailedPrecondition, "instrument specification is not loaded"};
    }
    if (!is_on_step(params_.quote_size, init.instrument.lot_size)) {
        // Caught here rather than left for the runtime to reject on every
        // single evaluation.
        return {ErrorCode::InvalidArgument, "quote_size is not on the venue's lot grid"};
    }

    instrument_ = init.instrument;
    initialized_ = true;
    fills_seen_ = 0;
    return Status::ok();
}

QuoteIntent ReferenceMarketMaker::decide(const StrategyContext& context) const {
    if (!initialized_) {
        return QuoteIntent::pull(IntentReason::Warmup);
    }
    if (!context.has_two_sided_market()) {
        return QuoteIntent::pull(IntentReason::InsufficientDepth);
    }

    const Px reference = params_.use_micro_price ? context.micro_price() : context.mid();
    if (!reference.is_positive()) {
        return QuoteIntent::pull(IntentReason::InsufficientDepth);
    }

    if (params_.min_market_spread_bps > 0.0) {
        const double market_spread_bps = 10'000.0 *
                                         static_cast<double>(context.spread().raw()) /
                                         static_cast<double>(reference.raw());
        if (market_spread_bps < params_.min_market_spread_bps) {
            return QuoteIntent::pull(IntentReason::SpreadTooTight);
        }
    }

    // Inventory pushes both quotes away from the side we are already long. A
    // linear skew is the simplest thing that responds at all; it is not a model.
    const double utilisation = context.inventory.utilisation();
    const double skew_bps = -utilisation * params_.inventory_skew_bps;

    QuoteIntent intent;
    intent.action = QuoteAction::Quote;
    intent.reason = IntentReason::Normal;
    intent.market_sequence = context.sequence;

    const Px bid_raw = shift_bps(reference, skew_bps - params_.half_spread_bps);
    const Px ask_raw = shift_bps(reference, skew_bps + params_.half_spread_bps);

    // Round outward: a bid rounds down and an ask rounds up, so rounding never
    // narrows the quoted spread into a worse price than intended.
    intent.bid_price = round_to_step(bid_raw, instrument_.tick_size, Rounding::Down);
    intent.ask_price = round_to_step(ask_raw, instrument_.tick_size, Rounding::Up);
    intent.bid_quantity = params_.quote_size;
    intent.ask_quantity = params_.quote_size;

    // Rounding can collapse a tight spread onto one tick; widen by a tick
    // rather than emit a crossed quote the runtime would reject.
    if (intent.bid_price >= intent.ask_price) {
        intent.ask_price = intent.bid_price + instrument_.tick_size;
    }

    // Stop adding to a side we are already heavy on. Quoting only the reducing
    // side is how a maker works its way back to flat.
    const double limit = params_.max_inventory_utilisation;
    intent.quote_bid = (utilisation < limit);
    intent.quote_ask = (utilisation > -limit);
    if (!intent.quote_bid && !intent.quote_ask) {
        return QuoteIntent::pull(IntentReason::InventoryLimit);
    }
    if (!intent.quote_bid || !intent.quote_ask) {
        intent.reason = IntentReason::InventoryLimit;
    }
    return intent;
}

QuoteIntent ReferenceMarketMaker::on_market_update(const StrategyContext& context) {
    return decide(context);
}

QuoteIntent ReferenceMarketMaker::on_timer(const StrategyContext& context) {
    // Identical logic. The quotes this strategy wants depend only on the market
    // and on inventory, never on how long it has been since the last one, so a
    // timer evaluation must produce the same answer as a market one.
    return decide(context);
}

void ReferenceMarketMaker::on_fill(const strategy::StrategyFill&) {
    // Position is supplied in the context on every evaluation, so there is
    // nothing to accumulate. Counting is enough to prove the callback arrives.
    ++fills_seen_;
}

void ReferenceMarketMaker::on_order_event(const strategy::StrategyOrderEvent&) {}

void ReferenceMarketMaker::reset() { fills_seen_ = 0; }

strategy::StrategyIdentity ReferenceMarketMaker::identity() const {
    strategy::StrategyIdentity id;
    static_cast<void>(id.name.assign(kName));
    id.version = kVersion;
    return id;
}

}  // namespace mm::strategies

MM_REGISTER_STRATEGY(mm::strategies::ReferenceMarketMaker, "reference_mm_v1", 1,
                     "TEST/REFERENCE ONLY - deterministic symmetric quotes; not a trading "
                     "strategy and not suitable for live use")

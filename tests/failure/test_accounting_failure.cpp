/// Phase 10 failure and invariant tests.
///
/// The governing question for every failure below is the one the phase brief
/// asks: *can this make the system believe it has LESS exposure than it
/// actually has?* Where the answer could be yes, the behaviour is to refuse and
/// report, never to approximate.

#include <gtest/gtest.h>

#include <limits>

#include "mm/portfolio/PortfolioAccountant.hpp"
#include "support/AccountingOracle.hpp"

namespace mm::portfolio {
namespace {

Px px(const char* v) { Px p; EXPECT_TRUE(Px::parse(v, p)); return p; }
Qty qty(const char* v) { Qty q; EXPECT_TRUE(Qty::parse(v, q)); return q; }
Notional fee(const char* v) { Notional n; EXPECT_TRUE(Notional::parse(v, n)); return n; }

exchange::FillEvent make_fill(const char* trade, Side side, const char* price,
                              const char* quantity, const char* fee_amount = "0") {
    exchange::FillEvent f;
    EXPECT_TRUE(f.trade_id.assign(trade));
    f.symbol = Symbol("BTCUSDT");
    f.side = side;
    f.price = px(price);
    f.quantity = qty(quantity);
    f.fee = fee(fee_amount);
    return f;
}

class AccountingFailureTest : public ::testing::Test {
protected:
    AccountingFailureTest() : clock(millis(1'000), millis(1'000)), accountant(config(), clock) {
        EXPECT_EQ(accountant.register_symbol(Symbol("BTCUSDT")), AccountingError::None);
    }
    static AccountingConfig config() {
        AccountingConfig c;
        c.max_symbols = 8;
        return c;
    }
    ManualClock clock;
    PortfolioAccountant accountant;
};

// ===========================================================================
// Idempotency
// ===========================================================================

TEST_F(AccountingFailureTest, DuplicateFillChangesNothingAtAll) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "2", "1")).ok());
    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    ASSERT_NE(a, nullptr);

    const Qty position = a->position();
    const Px average = a->average_price();
    const Notional realized = a->realized();
    const Notional fees = a->fees();
    const std::uint64_t fills = a->fill_count();

    for (int i = 0; i < 10; ++i) {
        const FillOutcome out = accountant.on_fill(make_fill("T1", Side::Buy, "100", "2", "1"));
        EXPECT_EQ(out.error, AccountingError::DuplicateFill);
        EXPECT_TRUE(out.applied_quantity.is_zero());
    }

    // Position, PnL, fees and trade count: all four must be untouched.
    EXPECT_EQ(a->position(), position);
    EXPECT_EQ(a->average_price(), average);
    EXPECT_EQ(a->realized(), realized);
    EXPECT_EQ(a->fees(), fees);
    EXPECT_EQ(a->fill_count(), fills);
    EXPECT_EQ(accountant.metrics().duplicate_fills, 10U);
}

TEST_F(AccountingFailureTest, ADuplicateIsNotAnErrorThatCorruptsTheLedger) {
    // A redelivered fill means nothing changed and nothing needed to. Every
    // other error means a fill we were told about was NOT incorporated, so the
    // ledger no longer matches reality.
    EXPECT_FALSE(corrupts_ledger(AccountingError::DuplicateFill));
    EXPECT_FALSE(corrupts_ledger(AccountingError::None));
    for (const AccountingError e :
         {AccountingError::InvalidQuantity, AccountingError::InvalidPrice,
          AccountingError::MissingTradeId, AccountingError::ArithmeticOverflow,
          AccountingError::UnknownSymbol, AccountingError::Faulted}) {
        EXPECT_TRUE(corrupts_ledger(e)) << to_string(e);
    }
}

// ===========================================================================
// Invalid input — all refused, none applied
// ===========================================================================

TEST_F(AccountingFailureTest, ZeroAndNegativeQuantityAreRefused) {
    exchange::FillEvent zero = make_fill("T1", Side::Buy, "100", "1");
    zero.quantity = Qty::from_raw(0);
    EXPECT_EQ(accountant.on_fill(zero).error, AccountingError::InvalidQuantity);

    exchange::FillEvent negative = make_fill("T2", Side::Buy, "100", "1");
    negative.quantity = Qty::from_raw(-100);
    EXPECT_EQ(accountant.on_fill(negative).error, AccountingError::InvalidQuantity);

    EXPECT_TRUE(accountant.find(Symbol("BTCUSDT"))->is_flat());
    EXPECT_EQ(accountant.find(Symbol("BTCUSDT"))->fill_count(), 0U);
}

TEST_F(AccountingFailureTest, InvalidPriceIsRefused) {
    exchange::FillEvent zero = make_fill("T1", Side::Buy, "100", "1");
    zero.price = Px::from_raw(0);
    EXPECT_EQ(accountant.on_fill(zero).error, AccountingError::InvalidPrice);

    exchange::FillEvent negative = make_fill("T2", Side::Buy, "100", "1");
    negative.price = Px::from_raw(-1);
    EXPECT_EQ(accountant.on_fill(negative).error, AccountingError::InvalidPrice);
    EXPECT_TRUE(accountant.find(Symbol("BTCUSDT"))->is_flat());
}

TEST_F(AccountingFailureTest, AFillWithoutATradeIdIsRefused) {
    exchange::FillEvent anonymous = make_fill("", Side::Buy, "100", "1");
    // Without an identity it cannot be deduplicated, so accepting it makes the
    // whole ledger unverifiable -- a later redelivery would double-count.
    EXPECT_EQ(accountant.on_fill(anonymous).error, AccountingError::MissingTradeId);
    EXPECT_TRUE(accountant.find(Symbol("BTCUSDT"))->is_flat());
}

TEST_F(AccountingFailureTest, ANegativeFeeIsRefusedRatherThanTreatedAsACost) {
    exchange::FillEvent rebate = make_fill("T1", Side::Buy, "100", "1");
    rebate.fee = Notional::from_raw(-500);
    EXPECT_FALSE(accountant.on_fill(rebate).ok());
    EXPECT_TRUE(accountant.find(Symbol("BTCUSDT"))->fees().is_zero());
}

TEST_F(AccountingFailureTest, AFillForAnUnregisteredSymbolIsRefusedNotAutoCreated) {
    exchange::FillEvent other = make_fill("T1", Side::Buy, "100", "1");
    other.symbol = Symbol("ETHUSDT");
    EXPECT_EQ(accountant.on_fill(other).error, AccountingError::UnknownSymbol);
    // Auto-registering would bury either a config error or somebody else's
    // trade on our account.
    EXPECT_EQ(accountant.symbol_count(), 1U);
}

// ===========================================================================
// Overflow — fails closed, never wraps
// ===========================================================================

TEST_F(AccountingFailureTest, NotionalOverflowIsRefusedAndFaultsTheAccount) {
    // A quantity and price whose product cannot be represented. Wrapping would
    // turn an enormous position into a small one -- the exact failure that
    // makes a system believe it has less exposure than it has.
    exchange::FillEvent huge = make_fill("T1", Side::Buy, "1", "1");
    huge.price = Px::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    huge.quantity = Qty::from_raw(std::numeric_limits<std::int64_t>::max() / 2);

    const FillOutcome out = accountant.on_fill(huge);
    EXPECT_EQ(out.error, AccountingError::ArithmeticOverflow);

    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    EXPECT_TRUE(a->faulted()) << "a ledger known to be wrong must stop accepting fills";
    EXPECT_EQ(accountant.metrics().overflows, 1U);

    // And every later fill is refused while faulted.
    EXPECT_EQ(accountant.on_fill(make_fill("T2", Side::Buy, "100", "1")).error,
              AccountingError::Faulted);
}

TEST_F(AccountingFailureTest, AFaultedAccountNeverReadsAsFlat) {
    exchange::FillEvent huge = make_fill("T1", Side::Buy, "1", "1");
    huge.price = Px::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    huge.quantity = Qty::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    static_cast<void>(accountant.on_fill(huge));

    // THE question: could this make risk believe there is no exposure?
    const risk::PositionSnapshot snapshot = accountant.position_snapshot(Symbol("BTCUSDT"));
    EXPECT_FALSE(snapshot.valid)
        << "an invalid position must be invalid, never a valid zero -- risk refuses to add "
           "exposure against a position it cannot vouch for";

    const PortfolioSnapshot portfolio = accountant.portfolio_snapshot();
    EXPECT_TRUE(portfolio.degraded);
    EXPECT_FALSE(portfolio.valid);
    EXPECT_FALSE(portfolio.pnl_determinate);
}

TEST_F(AccountingFailureTest, PartiallyAppliedFillsAreImpossible) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "5", "1")).ok());
    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    const Qty before_position = a->position();
    const Notional before_fees = a->fees();

    // A fill that overflows partway through must leave everything untouched --
    // a half-applied fill is worse than a rejected one because it looks valid.
    exchange::FillEvent huge = make_fill("T2", Side::Buy, "1", "1", "7");
    huge.price = Px::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    huge.quantity = Qty::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    EXPECT_EQ(accountant.on_fill(huge).error, AccountingError::ArithmeticOverflow);

    EXPECT_EQ(a->position(), before_position);
    EXPECT_EQ(a->fees(), before_fees) << "the fee must not be applied when the fill was not";
}

TEST_F(AccountingFailureTest, AFaultedAccountCanOnlyBeClearedDeliberately) {
    exchange::FillEvent huge = make_fill("T1", Side::Buy, "1", "1");
    huge.price = Px::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    huge.quantity = Qty::from_raw(std::numeric_limits<std::int64_t>::max() / 2);
    ASSERT_EQ(accountant.on_fill(huge).error, AccountingError::ArithmeticOverflow);
    ASSERT_TRUE(accountant.degraded());

    // Nothing clears it on its own -- not time, not further fills, not a mark.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(accountant.on_fill(make_fill(("R" + std::to_string(i)).c_str(), Side::Buy,
                                               "100", "1"))
                      .error,
                  AccountingError::Faulted);
    }
    clock.advance(seconds(3'600));
    EXPECT_TRUE(accountant.degraded()) << "a fault is sticky until an operator resolves it";

    // The operator path exists and works. Without it a faulted account would
    // be permanently stuck, which is a worse outcome than the fault.
    const_cast<PositionAccount*>(accountant.find(Symbol("BTCUSDT")))->clear_fault();
    EXPECT_FALSE(accountant.degraded());
    EXPECT_TRUE(accountant.on_fill(make_fill("AFTER", Side::Buy, "100", "1")).ok());
    EXPECT_TRUE(accountant.portfolio_snapshot().valid);
}

TEST_F(AccountingFailureTest, DuplicateDetectionDepthIsFixedAndSaysSoWhenExceeded) {
    // Regression: the config once carried a `dedup_capacity` field that sized
    // nothing. The depth is a compile-time property of the ring, and the
    // account reports when it has wrapped rather than silently losing the
    // ability to detect a redelivery.
    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    ASSERT_FALSE(a->dedup_overflowed());

    for (std::size_t i = 0; i < SeenTrades::kCapacity + 8; ++i) {
        ASSERT_TRUE(accountant
                        .on_fill(make_fill(("D" + std::to_string(i)).c_str(),
                                           i % 2 == 0 ? Side::Buy : Side::Sell, "100", "0.001"))
                        .ok());
    }
    EXPECT_TRUE(a->dedup_overflowed())
        << "'we may no longer detect a duplicate' must be visible, not assumed away";

    // The most recent ids are still recognised.
    const std::string recent = "D" + std::to_string(SeenTrades::kCapacity + 7);
    EXPECT_EQ(accountant.on_fill(make_fill(recent.c_str(), Side::Sell, "100", "0.001")).error,
              AccountingError::DuplicateFill);
}

// ===========================================================================
// Marks
// ===========================================================================

TEST_F(AccountingFailureTest, AStaleMarkMakesUnrealizedIndeterminateNotStale) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "1")).ok());
    MarkPrice fresh;
    fresh.symbol = Symbol("BTCUSDT");
    fresh.bid = px("110");
    fresh.ask = px("111");
    fresh.as_of_ns = clock.steady();
    ASSERT_EQ(accountant.on_mark(fresh), AccountingError::None);
    ASSERT_TRUE(accountant.find(Symbol("BTCUSDT"))->unrealized_valid());

    clock.advance(seconds(30));
    MarkPrice old = fresh;  // same timestamp, now far in the past
    EXPECT_EQ(accountant.on_mark(old), AccountingError::StaleMark);

    const PositionAccount* a = accountant.find(Symbol("BTCUSDT"));
    EXPECT_FALSE(a->unrealized_valid())
        << "an old number is not a small error in PnL; it is an unknown";
    EXPECT_TRUE(a->unrealized().is_zero());
    EXPECT_FALSE(accountant.portfolio_snapshot().pnl_determinate);
}

TEST_F(AccountingFailureTest, AnInvalidOrCrossedMarkIsRefused) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "1")).ok());
    for (const auto& [bid, ask] : std::vector<std::pair<const char*, const char*>>{
             {"0", "100"}, {"100", "0"}, {"110", "105"}}) {
        MarkPrice bad;
        bad.symbol = Symbol("BTCUSDT");
        bad.bid = px(bid);
        bad.ask = px(ask);
        bad.as_of_ns = clock.steady();
        EXPECT_EQ(accountant.on_mark(bad), AccountingError::InvalidMark) << bid << "/" << ask;
    }
    EXPECT_FALSE(accountant.find(Symbol("BTCUSDT"))->unrealized_valid());
}

TEST_F(AccountingFailureTest, NoMarkMeansGrossExposureFallsBackToCostNeverZero) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "3")).ok());
    // No mark has ever arrived. Reporting zero exposure would be the worst
    // possible answer; cost basis is the conservative fallback.
    const PortfolioSnapshot p = accountant.portfolio_snapshot();
    EXPECT_EQ(p.gross_notional.to_string(), "300");
    EXPECT_FALSE(p.pnl_determinate) << "PnL is unknown without a mark, but exposure is not";
    EXPECT_EQ(p.symbols_with_position, 1U);
}

// ===========================================================================
// Independent-oracle invariant tests
// ===========================================================================

/// Deterministic generated fill sequences, checked against a reimplementation
/// of the accounting model that shares no code with the engine.
TEST_F(AccountingFailureTest, PropertyMatchesAnIndependentOracle) {
    struct Xorshift {
        std::uint64_t s;
        std::uint64_t next() {
            s ^= s << 13U; s ^= s >> 7U; s ^= s << 17U; return s;
        }
    };

    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        PortfolioAccountant engine(config(), clock);
        ASSERT_EQ(engine.register_symbol(Symbol("BTCUSDT")), AccountingError::None);
        test::AccountingOracle oracle;
        Xorshift rng{seed * 0x9E3779B97F4A7C15ULL};

        Qty signed_sum{};
        std::size_t applied = 0;

        for (int step = 0; step < 40; ++step) {
            const bool buy = (rng.next() % 2) == 0;
            // Prices 90.00-109.75 in quarter steps; quantities 0.25-4.00.
            const auto price_ticks = static_cast<long long>(9'000 + (rng.next() % 80) * 25);
            const auto qty_ticks = static_cast<long long>(1 + (rng.next() % 16));
            const std::string price = std::to_string(price_ticks / 100) + "." +
                                      (price_ticks % 100 < 10 ? "0" : "") +
                                      std::to_string(price_ticks % 100);
            const std::string quantity = std::to_string(qty_ticks / 4) + "." +
                                         std::to_string((qty_ticks % 4) * 25);
            const std::string fee_amount = "0.0" + std::to_string(rng.next() % 9 + 1);
            const std::string trade = "P" + std::to_string(seed) + "_" + std::to_string(step);

            const FillOutcome out = engine.on_fill(make_fill(
                trade.c_str(), buy ? Side::Buy : Side::Sell, price.c_str(), quantity.c_str(),
                fee_amount.c_str()));
            ASSERT_TRUE(out.ok()) << to_string(out.error) << " at seed " << seed << " step " << step;
            ++applied;

            test::OracleFill of;
            of.buy = buy;
            of.price = std::stold(price);
            of.quantity = std::stold(quantity);
            of.fee = std::stold(fee_amount);
            oracle.apply(of);

            ASSERT_TRUE(checked_add(signed_sum, out.applied_quantity, signed_sum));
        }

        const PositionAccount* a = engine.find(Symbol("BTCUSDT"));
        ASSERT_NE(a, nullptr);

        // Invariant: position is exactly the sum of signed fills. This one is
        // exact -- no epsilon -- because it is pure fixed-point addition.
        EXPECT_EQ(a->position(), signed_sum) << "seed " << seed;

        const long double eps = test::oracle_epsilon(applied, 100.0L);
        EXPECT_NEAR(static_cast<double>(test::as_real(a->position())),
                    static_cast<double>(oracle.position()), static_cast<double>(eps))
            << "seed " << seed;
        EXPECT_NEAR(static_cast<double>(test::as_real(a->average_price())),
                    static_cast<double>(oracle.average()), static_cast<double>(eps))
            << "seed " << seed;
        EXPECT_NEAR(static_cast<double>(test::as_real(a->realized())),
                    static_cast<double>(oracle.realized()), static_cast<double>(eps))
            << "realized PnL diverged at seed " << seed;
        EXPECT_NEAR(static_cast<double>(test::as_real(a->fees())),
                    static_cast<double>(oracle.fees()), static_cast<double>(eps))
            << "seed " << seed;

        // And the mark-to-market agrees too.
        MarkPrice mark;
        mark.symbol = Symbol("BTCUSDT");
        mark.bid = px("101.00");
        mark.ask = px("101.50");
        mark.as_of_ns = clock.steady();
        ASSERT_EQ(engine.on_mark(mark), AccountingError::None);
        EXPECT_NEAR(static_cast<double>(test::as_real(a->unrealized())),
                    static_cast<double>(oracle.unrealized(101.00L, 101.50L)),
                    static_cast<double>(eps))
            << "unrealized PnL diverged at seed " << seed;
    }
}

TEST_F(AccountingFailureTest, PositionIsAlwaysTheSumOfSignedFillsEvenWithDuplicates) {
    Qty expected{};
    for (int i = 0; i < 30; ++i) {
        const bool buy = i % 3 != 0;
        const std::string trade = "S" + std::to_string(i);
        const FillOutcome out = accountant.on_fill(
            make_fill(trade.c_str(), buy ? Side::Buy : Side::Sell, "100", "0.5"));
        ASSERT_TRUE(out.ok());
        ASSERT_TRUE(checked_add(expected, out.applied_quantity, expected));

        // Every fill redelivered twice. The duplicates must contribute nothing.
        for (int d = 0; d < 2; ++d) {
            EXPECT_EQ(accountant
                          .on_fill(make_fill(trade.c_str(), buy ? Side::Buy : Side::Sell, "100",
                                             "0.5"))
                          .error,
                      AccountingError::DuplicateFill);
        }
        EXPECT_EQ(accountant.find(Symbol("BTCUSDT"))->position(), expected);
    }
}

// ===========================================================================
// Recovery
// ===========================================================================

TEST_F(AccountingFailureTest, SnapshotAndRestoreReproduceTheLedger) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "3", "1")).ok());
    ASSERT_TRUE(accountant.on_fill(make_fill("T2", Side::Sell, "110", "1", "1")).ok());

    const PortfolioAccountant::RecoveryState state = accountant.save();
    PortfolioAccountant restored(config(), clock);
    ASSERT_EQ(restored.restore(state), AccountingError::None);

    const PositionAccount* before = accountant.find(Symbol("BTCUSDT"));
    const PositionAccount* after = restored.find(Symbol("BTCUSDT"));
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->position(), before->position());
    EXPECT_EQ(after->average_price(), before->average_price());
    EXPECT_EQ(after->realized(), before->realized());
    EXPECT_EQ(after->fees(), before->fees());

    // Deduplication state survives, so a fill redelivered across the restart
    // is still recognised.
    EXPECT_EQ(restored.on_fill(make_fill("T1", Side::Buy, "100", "3", "1")).error,
              AccountingError::DuplicateFill);

    // Unrealized PnL is deliberately NOT restored: it is a function of a
    // market price, and the market has moved since the snapshot.
    EXPECT_FALSE(after->unrealized_valid());
}

TEST_F(AccountingFailureTest, ACorruptSnapshotIsRefusedNotAdopted) {
    PortfolioAccountant::RecoveryState state;
    PositionAccount::State bad;
    bad.symbol = Symbol("BTCUSDT");
    bad.position = qty("5");
    bad.average_price = Px::from_raw(0);  // a position with no cost basis
    state.accounts.push_back(bad);

    PortfolioAccountant target(config(), clock);
    EXPECT_EQ(target.restore(state), AccountingError::CorruptSnapshot);
    // Nothing partially restored: a half-restored ledger looks complete.
    EXPECT_EQ(target.symbol_count(), 0U);

    PortfolioAccountant::RecoveryState flat_with_basis;
    PositionAccount::State odd;
    odd.symbol = Symbol("BTCUSDT");
    odd.position = Qty::from_raw(0);
    odd.average_price = px("100");  // flat but carrying a basis
    flat_with_basis.accounts.push_back(odd);
    PortfolioAccountant target2(config(), clock);
    EXPECT_EQ(target2.restore(flat_with_basis), AccountingError::CorruptSnapshot);
}

TEST_F(AccountingFailureTest, RestoreIntoANonEmptyLedgerIsRefused) {
    ASSERT_TRUE(accountant.on_fill(make_fill("T1", Side::Buy, "100", "1")).ok());
    EXPECT_EQ(accountant.restore(PortfolioAccountant::RecoveryState{}),
              AccountingError::CorruptSnapshot);
}

// ===========================================================================
// Multi-symbol
// ===========================================================================

TEST_F(AccountingFailureTest, SymbolsAreAccountedIndependentlyAndAggregatedAbsolutely) {
    ASSERT_EQ(accountant.register_symbol(Symbol("ETHUSDT")), AccountingError::None);

    ASSERT_TRUE(accountant.on_fill(make_fill("B1", Side::Buy, "100", "2")).ok());
    exchange::FillEvent eth = make_fill("E1", Side::Sell, "50", "4");
    eth.symbol = Symbol("ETHUSDT");
    ASSERT_TRUE(accountant.on_fill(eth).ok());

    EXPECT_EQ(accountant.find(Symbol("BTCUSDT"))->position().to_string(), "2");
    EXPECT_EQ(accountant.find(Symbol("ETHUSDT"))->position().to_string(), "-4");

    const PortfolioSnapshot p = accountant.portfolio_snapshot();
    // Gross is absolute: a long and a short do not net to "no exposure".
    EXPECT_EQ(p.gross_notional.to_string(), "400");
    // Net is a different question, kept separately: 200 long - 200 short = 0.
    EXPECT_EQ(p.net_notional.to_string(), "0");
    EXPECT_EQ(p.symbols_with_position, 2U);
}

TEST_F(AccountingFailureTest, CapacityIsRefusedNotGrown) {
    for (int i = 0; i < 16; ++i) {
        const std::string s = "SYM" + std::to_string(i);
        const AccountingError e = accountant.register_symbol(Symbol(s.c_str()));
        if (i < 7) {
            EXPECT_EQ(e, AccountingError::None) << i;
        } else {
            EXPECT_EQ(e, AccountingError::CapacityExhausted) << i;
        }
    }
    EXPECT_EQ(accountant.symbol_count(), 8U);
}

// ===========================================================================
// Balances (§24 — state representation only; reconciliation is Phase 11)
// ===========================================================================

TEST_F(AccountingFailureTest, BalancesAreRecordedAndNegativeOnesRefused) {
    exchange::BalanceUpdateEvent b;
    EXPECT_TRUE(b.asset.assign("USDT"));
    EXPECT_TRUE(Qty::parse("1000", b.free));
    EXPECT_TRUE(Qty::parse("250", b.locked));
    EXPECT_EQ(accountant.on_balance(b), AccountingError::None);

    const AssetBalance* stored = accountant.balance(Asset("USDT"));
    ASSERT_NE(stored, nullptr);
    Qty total;
    ASSERT_TRUE(stored->checked_total(total));
    EXPECT_EQ(total.to_string(), "1250");

    exchange::BalanceUpdateEvent negative = b;
    negative.free = Qty::from_raw(-1);
    // A negative balance is not a small balance; it is a message we cannot
    // interpret.
    EXPECT_EQ(accountant.on_balance(negative), AccountingError::InvalidQuantity);
    EXPECT_EQ(accountant.balance(Asset("USDT"))->free.to_string(), "1000");
}

}  // namespace
}  // namespace mm::portfolio

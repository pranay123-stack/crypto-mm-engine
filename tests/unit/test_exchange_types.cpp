#include <gtest/gtest.h>

#include <vector>

#include "mm/exchange/common/ExchangeTypes.hpp"

namespace mm::exchange {
namespace {

constexpr SessionState kAllSessionStates[] = {
    SessionState::Disconnected, SessionState::Connecting,     SessionState::Connected,
    SessionState::Authenticated, SessionState::Subscribed,    SessionState::Syncing,
    SessionState::Ready,        SessionState::Stale,          SessionState::ResyncRequired,
    SessionState::Backoff,      SessionState::Error,
};

constexpr OrderStatus kAllOrderStatuses[] = {
    OrderStatus::Unknown,       OrderStatus::New,           OrderStatus::PartiallyFilled,
    OrderStatus::Filled,        OrderStatus::Canceled,      OrderStatus::Rejected,
    OrderStatus::Expired,       OrderStatus::PendingCancel, OrderStatus::PendingReplace,
};

// ---------------------------------------------------------------- order status

TEST(OrderStatus, TerminalClassification) {
    EXPECT_TRUE(is_terminal(OrderStatus::Filled));
    EXPECT_TRUE(is_terminal(OrderStatus::Canceled));
    EXPECT_TRUE(is_terminal(OrderStatus::Rejected));
    EXPECT_TRUE(is_terminal(OrderStatus::Expired));

    EXPECT_FALSE(is_terminal(OrderStatus::New));
    EXPECT_FALSE(is_terminal(OrderStatus::PartiallyFilled));
    EXPECT_FALSE(is_terminal(OrderStatus::PendingCancel));
    EXPECT_FALSE(is_terminal(OrderStatus::PendingReplace));
}

TEST(OrderStatus, UnknownIsTreatedAsPossiblyLive) {
    // "We know nothing" must never be optimised into "there is no order". The
    // expensive mistake is assuming an order is gone when it is still working.
    EXPECT_FALSE(is_terminal(OrderStatus::Unknown));
    EXPECT_TRUE(may_still_trade(OrderStatus::Unknown));
}

TEST(OrderStatus, TerminalAndTradableArePreciseComplements) {
    for (const auto s : kAllOrderStatuses) {
        EXPECT_NE(is_terminal(s), may_still_trade(s)) << to_string(s);
    }
}

TEST(OrderStatus, EveryValueHasAName) {
    for (const auto s : kAllOrderStatuses) {
        if (s == OrderStatus::Unknown) {
            continue;
        }
        EXPECT_NE(to_string(s), "UNKNOWN");
    }
}

// ---------------------------------------------------------------- session

TEST(SessionState, OnlyReadyPermitsQuoting) {
    for (const auto s : kAllSessionStates) {
        EXPECT_EQ(is_quotable(s), s == SessionState::Ready) << to_string(s);
    }
}

TEST(SessionState, StaleIsNotQuotable) {
    // A socket that is open while data has stopped is the failure mode that
    // quietly loses money: the book looks fine and is minutes old.
    EXPECT_FALSE(is_quotable(SessionState::Stale));
}

TEST(SessionState, HappyPathIsLegalEndToEnd) {
    const std::vector<SessionState> path = {
        SessionState::Disconnected, SessionState::Connecting, SessionState::Connected,
        SessionState::Authenticated, SessionState::Subscribed, SessionState::Syncing,
        SessionState::Ready,
    };
    for (std::size_t i = 1; i < path.size(); ++i) {
        EXPECT_TRUE(is_legal_transition(path[i - 1], path[i]))
            << to_string(path[i - 1]) << " -> " << to_string(path[i]);
    }
}

TEST(SessionState, PublicMarketDataMaySkipAuthentication) {
    EXPECT_TRUE(is_legal_transition(SessionState::Connected, SessionState::Subscribed));
}

TEST(SessionState, SubscriptionAloneNeverMakesABookQuotable) {
    // The single most important illegal transition: being subscribed says
    // nothing about whether a snapshot has been applied. Allowing it would let
    // a strategy quote against an empty book.
    EXPECT_FALSE(is_legal_transition(SessionState::Subscribed, SessionState::Ready));
    EXPECT_FALSE(is_legal_transition(SessionState::Connected, SessionState::Ready));
    EXPECT_FALSE(is_legal_transition(SessionState::Disconnected, SessionState::Ready));
    EXPECT_FALSE(is_legal_transition(SessionState::Backoff, SessionState::Ready));
    EXPECT_FALSE(is_legal_transition(SessionState::ResyncRequired, SessionState::Ready));
}

TEST(SessionState, ResyncMustGoBackThroughSyncing) {
    EXPECT_TRUE(is_legal_transition(SessionState::ResyncRequired, SessionState::Syncing));
    EXPECT_TRUE(is_legal_transition(SessionState::Syncing, SessionState::Ready));
    EXPECT_FALSE(is_legal_transition(SessionState::ResyncRequired, SessionState::Ready));
}

TEST(SessionState, StaleRecoversOnlyToReadyOrResync) {
    EXPECT_TRUE(is_legal_transition(SessionState::Stale, SessionState::Ready));
    EXPECT_TRUE(is_legal_transition(SessionState::Stale, SessionState::ResyncRequired));
    EXPECT_FALSE(is_legal_transition(SessionState::Stale, SessionState::Syncing));
    EXPECT_FALSE(is_legal_transition(SessionState::Stale, SessionState::Subscribed));
}

TEST(SessionState, DisconnectAndErrorAreReachableFromAnywhere) {
    for (const auto from : kAllSessionStates) {
        EXPECT_TRUE(is_legal_transition(from, SessionState::Disconnected)) << to_string(from);
        EXPECT_TRUE(is_legal_transition(from, SessionState::Error)) << to_string(from);
    }
}

TEST(SessionState, ErrorNeverSelfHealsIntoAQuotableState) {
    // Only an operator clears an Error, and only by tearing the session down.
    for (const auto to : kAllSessionStates) {
        if (to == SessionState::Error || to == SessionState::Disconnected) {
            continue;
        }
        EXPECT_FALSE(is_legal_transition(SessionState::Error, to))
            << "Error -> " << to_string(to) << " must not be legal";
    }
}

TEST(SessionState, BackoffIsUnreachableFromDisconnected) {
    // From Disconnected the next step is an attempt, not a wait.
    EXPECT_FALSE(is_legal_transition(SessionState::Disconnected, SessionState::Backoff));
    EXPECT_TRUE(is_legal_transition(SessionState::Connecting, SessionState::Backoff));
    EXPECT_TRUE(is_legal_transition(SessionState::Ready, SessionState::Backoff));
}

TEST(SessionState, SelfTransitionsAreIdempotentNotViolations) {
    // Adapters legitimately re-announce state after a heartbeat.
    for (const auto s : kAllSessionStates) {
        EXPECT_TRUE(is_legal_transition(s, s)) << to_string(s);
    }
}

TEST(SessionState, ReadyIsOnlyReachableFromSyncingOrStale) {
    for (const auto from : kAllSessionStates) {
        const bool legal = is_legal_transition(from, SessionState::Ready);
        const bool expected = (from == SessionState::Syncing || from == SessionState::Stale ||
                               from == SessionState::Ready);
        EXPECT_EQ(legal, expected) << to_string(from) << " -> Ready";
    }
}

TEST(SessionState, EveryValueHasAName) {
    for (const auto s : kAllSessionStates) {
        EXPECT_NE(to_string(s), "UNKNOWN");
    }
}

// ---------------------------------------------------------------- connection

TEST(ConnectionState, OnlyConnectedIsUsable) {
    EXPECT_TRUE(is_usable(ConnectionState::Connected));
    for (const auto s : {ConnectionState::Disconnected, ConnectionState::Connecting,
                         ConnectionState::Reconnecting, ConnectionState::Failed}) {
        EXPECT_FALSE(is_usable(s));
    }
}

// ---------------------------------------------------------------- outcomes

TEST(RequestOutcome, ReconciliationIsRequiredExactlyForUnknown) {
    EXPECT_TRUE(requires_reconciliation(RequestOutcome::Unknown));
    EXPECT_FALSE(requires_reconciliation(RequestOutcome::Acknowledged));
    EXPECT_FALSE(requires_reconciliation(RequestOutcome::Rejected));
    EXPECT_FALSE(requires_reconciliation(RequestOutcome::NotSent));
}

TEST(RequestOutcome, DefinitiveAbsenceIsRejectedOrNotSent) {
    EXPECT_TRUE(is_definitively_absent(RequestOutcome::Rejected));
    EXPECT_TRUE(is_definitively_absent(RequestOutcome::NotSent));
    // The two that matter: neither an acknowledged order nor an unknown one may
    // ever be treated as absent.
    EXPECT_FALSE(is_definitively_absent(RequestOutcome::Unknown));
    EXPECT_FALSE(is_definitively_absent(RequestOutcome::Acknowledged));
}

TEST(CancelStatus, RejectedCancelLeavesTheOrderWorking) {
    // Semantic anchor: a cancel reject is not an order termination. Exposure
    // must not be released on it.
    EXPECT_NE(CancelStatus::Rejected, CancelStatus::Accepted);
    EXPECT_EQ(to_string(CancelStatus::Rejected), "REJECTED");
    EXPECT_EQ(to_string(CancelStatus::TooLate), "TOO_LATE");
}

}  // namespace
}  // namespace mm::exchange

#include <gtest/gtest.h>

#include <vector>

#include "mm/exchange/common/ExchangeError.hpp"

namespace mm::exchange {
namespace {

constexpr ExchangeErrorCategory kAllCategories[] = {
    ExchangeErrorCategory::None,
    ExchangeErrorCategory::Transport,
    ExchangeErrorCategory::Authentication,
    ExchangeErrorCategory::InvalidRequest,
    ExchangeErrorCategory::RateLimit,
    ExchangeErrorCategory::ExchangeRejection,
    ExchangeErrorCategory::Timeout,
    ExchangeErrorCategory::ConnectionFailure,
    ExchangeErrorCategory::UnknownOrderState,
    ExchangeErrorCategory::UnsupportedOperation,
    ExchangeErrorCategory::MalformedResponse,
};

constexpr RequestOutcome kAllOutcomes[] = {
    RequestOutcome::Acknowledged,
    RequestOutcome::Rejected,
    RequestOutcome::Unknown,
    RequestOutcome::NotSent,
};

// ---------------------------------------------------------------------------
// The invariant this whole file exists to protect.
// ---------------------------------------------------------------------------

TEST(ExchangeError, SilenceFromTheVenueCanNeverMeanRejected) {
    // If the venue never told us it refused the order, nothing may claim it
    // did. Treating a timeout as a rejection is how a market maker ends up with
    // a live order it believes does not exist.
    for (const auto category : {ExchangeErrorCategory::Timeout,
                                ExchangeErrorCategory::Transport,
                                ExchangeErrorCategory::ConnectionFailure,
                                ExchangeErrorCategory::UnknownOrderState,
                                ExchangeErrorCategory::MalformedResponse}) {
        EXPECT_FALSE(is_consistent(category, RequestOutcome::Rejected))
            << to_string(category) << " must never imply the venue rejected the order";

        // And an adapter that tries anyway is corrected rather than believed.
        const ExchangeError e =
            ExchangeError::make(category, RequestOutcome::Rejected, "adapter bug");
        EXPECT_NE(e.outcome, RequestOutcome::Rejected)
            << to_string(category) << " was allowed to report Rejected";
        EXPECT_FALSE(e.order_definitively_absent());
    }
}

TEST(ExchangeError, CorrectionOfABuggyAdapterErrsTowardUncertainty) {
    // When an adapter offers an impossible pairing, the correction must never
    // be *less* safe than what it offered. ConnectionFailure's typical outcome
    // is NotSent ("forget the order"), but a confused adapter earns Unknown
    // ("go and check"), because forgetting a live order is unrecoverable and a
    // needless reconciliation is not.
    const ExchangeError e = ExchangeError::make(ExchangeErrorCategory::ConnectionFailure,
                                                RequestOutcome::Rejected, "adapter bug");
    EXPECT_EQ(e.outcome, RequestOutcome::Unknown);
    EXPECT_TRUE(e.needs_reconciliation());
    EXPECT_FALSE(e.order_definitively_absent());

    // A category with no legal Unknown still falls back to its default, which
    // is correct there: we genuinely never sent it.
    const ExchangeError unsupported = ExchangeError::make(
        ExchangeErrorCategory::UnsupportedOperation, RequestOutcome::Rejected, "adapter bug");
    EXPECT_EQ(unsupported.outcome, RequestOutcome::NotSent);
}

TEST(ExchangeError, ExplicitlyStatedLegalOutcomesArePreserved) {
    // Correction must only engage for impossible pairings; a deliberate,
    // legal NotSent must survive.
    const ExchangeError e = ExchangeError::make(ExchangeErrorCategory::ConnectionFailure,
                                                RequestOutcome::NotSent, "never written");
    EXPECT_EQ(e.outcome, RequestOutcome::NotSent);
}

TEST(ExchangeError, TimeoutIsAlwaysUnknown) {
    const ExchangeError e = ExchangeError::from_category(ExchangeErrorCategory::Timeout);
    EXPECT_EQ(e.outcome, RequestOutcome::Unknown);
    EXPECT_TRUE(e.needs_reconciliation());
    EXPECT_FALSE(e.order_definitively_absent());
}

TEST(ExchangeError, TimeoutCannotBeNotSent) {
    // A timeout means the request WAS sent; claiming otherwise would let the
    // OMS discard an order that may be resting on the venue right now.
    EXPECT_FALSE(is_consistent(ExchangeErrorCategory::Timeout, RequestOutcome::NotSent));
}

TEST(ExchangeError, OnlyVenueRejectionYieldsDefinitiveAbsenceViaRejection) {
    for (const auto category : kAllCategories) {
        if (category == ExchangeErrorCategory::None) {
            continue;
        }
        if (is_consistent(category, RequestOutcome::Rejected)) {
            // The only categories permitted to say "rejected" are those where
            // the venue actively responded.
            EXPECT_TRUE(category == ExchangeErrorCategory::ExchangeRejection ||
                        category == ExchangeErrorCategory::InvalidRequest ||
                        category == ExchangeErrorCategory::RateLimit ||
                        category == ExchangeErrorCategory::Authentication)
                << to_string(category) << " should not be able to report Rejected";
        }
    }
}

TEST(ExchangeError, SuccessIsNeverAnError) {
    for (const auto category : kAllCategories) {
        if (category == ExchangeErrorCategory::None) {
            EXPECT_TRUE(is_consistent(category, RequestOutcome::Acknowledged));
            continue;
        }
        EXPECT_FALSE(is_consistent(category, RequestOutcome::Acknowledged))
            << to_string(category) << " must not report success";
    }
}

TEST(ExchangeError, NoneRequiresAcknowledged) {
    EXPECT_TRUE(is_consistent(ExchangeErrorCategory::None, RequestOutcome::Acknowledged));
    for (const auto outcome : {RequestOutcome::Rejected, RequestOutcome::Unknown,
                               RequestOutcome::NotSent}) {
        EXPECT_FALSE(is_consistent(ExchangeErrorCategory::None, outcome));
    }
}

TEST(ExchangeError, EveryDefaultOutcomeIsSelfConsistent) {
    // The conservative default must itself be a legal pairing, or `make` would
    // fall back to something illegal when correcting an adapter.
    for (const auto category : kAllCategories) {
        const RequestOutcome outcome = default_outcome_for(category);
        EXPECT_TRUE(is_consistent(category, outcome))
            << to_string(category) << " default outcome " << to_string(outcome)
            << " is not a legal pairing";
    }
}

TEST(ExchangeError, DefaultsErrOnTheSideOfUncertainty) {
    // Where a category is ambiguous, the default must be Unknown -- the
    // direction whose worst case is a needless reconciliation rather than an
    // untracked live order.
    for (const auto category : {ExchangeErrorCategory::Authentication,
                                ExchangeErrorCategory::RateLimit,
                                ExchangeErrorCategory::InvalidRequest}) {
        EXPECT_EQ(default_outcome_for(category), RequestOutcome::Unknown)
            << to_string(category) << " should default to Unknown when ambiguous";
    }
}

TEST(ExchangeError, LocallyRefusedRequestsNeedNoReconciliation) {
    for (const auto category : {ExchangeErrorCategory::UnsupportedOperation,
                                ExchangeErrorCategory::ConnectionFailure}) {
        const ExchangeError e = ExchangeError::from_category(category);
        EXPECT_EQ(e.outcome, RequestOutcome::NotSent);
        EXPECT_FALSE(e.needs_reconciliation());
        EXPECT_TRUE(e.order_definitively_absent());
    }
}

TEST(ExchangeError, ConnectionFailureCanBeEitherUnknownOrNotSent) {
    // Died mid-write, or never got written at all. The adapter knows which.
    EXPECT_TRUE(is_consistent(ExchangeErrorCategory::ConnectionFailure, RequestOutcome::Unknown));
    EXPECT_TRUE(is_consistent(ExchangeErrorCategory::ConnectionFailure, RequestOutcome::NotSent));
    EXPECT_FALSE(is_consistent(ExchangeErrorCategory::ConnectionFailure, RequestOutcome::Rejected));
}

TEST(ExchangeError, ConsistencyMatrixIsTotal) {
    // Every pairing has a defined answer; none crashes or falls through.
    std::size_t legal = 0;
    for (const auto category : kAllCategories) {
        for (const auto outcome : kAllOutcomes) {
            if (is_consistent(category, outcome)) {
                ++legal;
            }
        }
    }
    EXPECT_GT(legal, 0U);
    EXPECT_LT(legal, std::size(kAllCategories) * std::size(kAllOutcomes))
        << "if every pairing were legal the consistency check would be vacuous";
}

// ---------------------------------------------------------------------------
// Construction and reporting
// ---------------------------------------------------------------------------

TEST(ExchangeError, NoneIsNotAnError) {
    const ExchangeError e = ExchangeError::none();
    EXPECT_FALSE(e.is_error());
    EXPECT_EQ(e.category, ExchangeErrorCategory::None);
}

TEST(ExchangeError, CarriesVenueCodeWithoutExposingItToLogic) {
    const ExchangeError e =
        ExchangeError::make(ExchangeErrorCategory::ExchangeRejection, RequestOutcome::Rejected,
                            "insufficient balance", RejectReason::InsufficientBalance, -2010);
    EXPECT_EQ(e.venue_code, -2010);
    EXPECT_EQ(e.reason, RejectReason::InsufficientBalance);
    // The normalized reason is what callers branch on; the raw code is for logs.
    EXPECT_NE(e.to_string().find("INSUFFICIENT_BALANCE"), std::string::npos);
    EXPECT_NE(e.to_string().find("-2010"), std::string::npos);
}

TEST(ExchangeError, ToStringIncludesCategoryAndOutcome) {
    const ExchangeError e = ExchangeError::from_category(ExchangeErrorCategory::Timeout, "5s");
    const std::string s = e.to_string();
    EXPECT_NE(s.find("TIMEOUT"), std::string::npos);
    EXPECT_NE(s.find("UNKNOWN"), std::string::npos);
    EXPECT_NE(s.find("5s"), std::string::npos);
}

TEST(ExchangeError, IsRingTransportable) {
    static_assert(std::is_trivially_copyable_v<ExchangeError>);
    SUCCEED();
}

TEST(ExchangeError, EveryCategoryHasAName) {
    for (const auto category : kAllCategories) {
        if (category == ExchangeErrorCategory::None) {
            continue;
        }
        EXPECT_NE(to_string(category), "UNKNOWN");
    }
}

}  // namespace
}  // namespace mm::exchange

#pragma once

/// \file OrderRequest.hpp
/// Normalized outbound requests, and the validation every one passes before it
/// is allowed near a socket.
///
/// Pre-validating locally is not defensive decoration. A rejected order costs a
/// round trip and a rate-limit slot, and a burst of rejects is indistinguishable
/// from a venue outage to the safety layer -- so an order the venue is certain
/// to refuse must never be sent in the first place.

#include "mm/common/Types.hpp"
#include "mm/exchange/common/ExchangeCapabilities.hpp"
#include "mm/exchange/common/ExchangeError.hpp"

namespace mm::exchange {

struct OrderRequest {
    /// Ours, and the join key across the whole traceability chain. Generated to
    /// be unique across process restarts.
    ClientOrderId client_order_id{};

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;

    /// Ignored for OrderType::Market, and required to be zero there so a stale
    /// price cannot ride along unnoticed.
    Px price{};
    Qty quantity{};

    /// Refuse rather than cross. `OrderType::LimitMaker` means the same thing;
    /// `is_post_only()` is the single predicate that accounts for both spellings.
    bool post_only = false;
    /// Derivatives venues only; validated against capabilities.
    bool reduce_only = false;

    /// Correlates this order with the strategy decision that produced it.
    TraceId trace = kNoTrace;
    /// Steady clock, stamped when the request was created. The ack timeout is
    /// measured from here, so it must be monotonic.
    Nanos created_ns = 0;

    [[nodiscard]] constexpr bool is_post_only() const noexcept {
        return post_only || type == OrderType::LimitMaker;
    }
    [[nodiscard]] constexpr bool requires_price() const noexcept {
        return type != OrderType::Market;
    }
    [[nodiscard]] Notional notional() const noexcept { return notional_of(price, quantity); }
};

static_assert(std::is_trivially_copyable_v<OrderRequest>);

struct CancelRequest {
    /// The order being cancelled. At least one of these must identify it; the
    /// exchange ID is preferred when known because it survives our own ID
    /// scheme changing.
    ClientOrderId client_order_id{};
    ExchangeOrderId exchange_order_id{};

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    TraceId trace = kNoTrace;
    Nanos created_ns = 0;

    [[nodiscard]] constexpr bool identifies_an_order() const noexcept {
        return !client_order_id.empty() || !exchange_order_id.empty();
    }
};

static_assert(std::is_trivially_copyable_v<CancelRequest>);

struct ReplaceRequest {
    /// The order being replaced.
    ClientOrderId original_client_order_id{};
    ExchangeOrderId original_exchange_order_id{};

    /// The replacement's identity. Always a fresh ID: reusing the original's
    /// would make the two indistinguishable in reports arriving out of order.
    ClientOrderId new_client_order_id{};

    Symbol symbol{};
    SymbolId symbol_id = SymbolId::kInvalid;

    Px new_price{};
    Qty new_quantity{};

    TraceId trace = kNoTrace;
    Nanos created_ns = 0;
};

static_assert(std::is_trivially_copyable_v<ReplaceRequest>);

/// Validates a request against the instrument's trading rules and the venue's
/// capabilities. Returns `ExchangeError::none()` when the request may be sent.
///
/// Every failure is reported as `InvalidRequest` with `RequestOutcome::NotSent`
/// -- the request never left this process, so there is nothing to reconcile.
[[nodiscard]] ExchangeError validate_order(const OrderRequest& req, const InstrumentSpec& spec,
                                           const ExchangeCapabilities& caps) noexcept;

[[nodiscard]] ExchangeError validate_cancel(const CancelRequest& req,
                                            const ExchangeCapabilities& caps) noexcept;

[[nodiscard]] ExchangeError validate_replace(const ReplaceRequest& req, const InstrumentSpec& spec,
                                             const ExchangeCapabilities& caps) noexcept;

}  // namespace mm::exchange

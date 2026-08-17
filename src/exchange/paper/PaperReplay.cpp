#include "mm/exchange/paper/PaperReplay.hpp"

#include <algorithm>

namespace mm::exchange::paper {
namespace {

/// Collects events into a comparable transcript.
class TranscribingSink final : public IExecutionSink {
public:
    void on_execution(const ExecutionEvent& event) override {
        std::string line(to_string(event.type));
        line += "|" + event.client_order_id().to_string();
        switch (event.type) {
            case ExecutionEventType::OrderAck:
                line += "|" + event.payload.ack.exchange_order_id.to_string() + "|" +
                        event.payload.ack.price.to_string() + "|" +
                        event.payload.ack.original_qty.to_string();
                break;
            case ExecutionEventType::Fill:
                line += "|" + event.payload.fill.trade_id.to_string() + "|" +
                        event.payload.fill.price.to_string() + "|" +
                        event.payload.fill.quantity.to_string() + "|" +
                        event.payload.fill.cumulative_qty.to_string();
                break;
            case ExecutionEventType::OrderCancel:
                line += std::string("|") + std::string(to_string(event.payload.cancel.cancel_status)) +
                        "|" + std::string(to_string(event.payload.cancel.order_status));
                break;
            case ExecutionEventType::OrderReplace:
                line += std::string("|") + (event.payload.replace.accepted ? "OK" : "NO") + "|" +
                        event.payload.replace.new_price.to_string();
                break;
            case ExecutionEventType::OrderReject:
                line += std::string("|") + std::string(to_string(event.payload.reject.error.reason));
                break;
            case ExecutionEventType::Connection:
                line += std::string("|") + std::string(to_string(event.payload.connection.to));
                break;
            case ExecutionEventType::OpenOrdersSnapshot:
                line += "|" + std::to_string(event.payload.open_orders.count);
                break;
            default:
                break;
        }
        transcript.push_back(std::move(line));
    }

    std::vector<std::string> transcript;
};

}  // namespace

bool ReplayResult::matches(const ReplayResult& other) const {
    if (transcript != other.transcript) {
        return false;
    }
    if (final_orders.size() != other.final_orders.size()) {
        return false;
    }
    for (std::size_t i = 0; i < final_orders.size(); ++i) {
        const PaperOrder& a = final_orders[i];
        const PaperOrder& b = other.final_orders[i];
        if (a.exchange_order_id != b.exchange_order_id || a.client_order_id != b.client_order_id ||
            a.status != b.status || a.price != b.price || a.original_qty != b.original_qty ||
            a.cumulative_qty != b.cumulative_qty || a.arrival_sequence != b.arrival_sequence) {
            return false;
        }
    }
    return true;
}

std::string ReplayResult::first_difference(const ReplayResult& other) const {
    const std::size_t n = std::min(transcript.size(), other.transcript.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (transcript[i] != other.transcript[i]) {
            return "event " + std::to_string(i) + ": '" + transcript[i] + "' vs '" +
                   other.transcript[i] + "'";
        }
    }
    if (transcript.size() != other.transcript.size()) {
        return "event count " + std::to_string(transcript.size()) + " vs " +
               std::to_string(other.transcript.size());
    }
    if (final_orders.size() != other.final_orders.size()) {
        return "final order count " + std::to_string(final_orders.size()) + " vs " +
               std::to_string(other.final_orders.size());
    }
    for (std::size_t i = 0; i < final_orders.size(); ++i) {
        if (final_orders[i].cumulative_qty != other.final_orders[i].cumulative_qty) {
            return "final cumulative quantity for " + final_orders[i].client_order_id.to_string();
        }
        if (final_orders[i].status != other.final_orders[i].status) {
            return "final status for " + final_orders[i].client_order_id.to_string();
        }
    }
    return {};
}

ReplayResult replay(const RecordedSession& session) {
    // Its own clock, from a fixed origin. Sharing the caller's would make the
    // result depend on how much time had passed before the replay began.
    ManualClock clock(0, 0);
    TranscribingSink sink;
    PaperExecution venue(session.config, clock);

    book::OrderBook book(session.symbol, 32);
    book.begin_snapshot();
    static_cast<void>(book.add_snapshot_levels(session.initial_levels));
    static_cast<void>(book.finish_snapshot(session.initial_update_id));

    static_cast<void>(venue.start(sink));
    venue.attach_book(session.symbol, book);
    venue.on_market_update(session.symbol, clock.steady());

    Nanos applied_ns = 0;
    for (const RecordedAction& action : session.actions) {
        // Offsets are absolute from session start, so replaying them in order
        // reproduces the original spacing exactly.
        if (action.offset_ns > applied_ns) {
            clock.advance(action.offset_ns - applied_ns);
            applied_ns = action.offset_ns;
        }
        switch (action.kind) {
            case RecordedAction::Kind::Submit:
                static_cast<void>(venue.submit(action.submit));
                break;
            case RecordedAction::Kind::Cancel:
                static_cast<void>(venue.cancel(action.cancel));
                break;
            case RecordedAction::Kind::Replace:
                static_cast<void>(venue.replace(action.replace));
                break;
            case RecordedAction::Kind::MarketUpdate: {
                book.begin_snapshot();
                static_cast<void>(book.add_snapshot_levels(action.levels));
                static_cast<void>(book.finish_snapshot(action.update_id));
                venue.on_market_update(action.symbol, clock.steady());
                break;
            }
            case RecordedAction::Kind::Poll:
                static_cast<void>(venue.poll());
                break;
        }
    }

    ReplayResult result;
    result.transcript = std::move(sink.transcript);
    result.metrics = venue.metrics();
    result.final_orders = venue.snapshot_orders();
    return result;
}

SessionRecorder::SessionRecorder(PaperExecutionConfig config, const Symbol& symbol,
                                 BookLevels initial, Seq initial_update_id) {
    session_.config = std::move(config);
    session_.symbol = symbol;
    session_.initial_levels = initial;
    session_.initial_update_id = initial_update_id;
}

void SessionRecorder::record_submit(Nanos offset_ns, const OrderRequest& request) {
    RecordedAction a;
    a.kind = RecordedAction::Kind::Submit;
    a.offset_ns = offset_ns;
    a.submit = request;
    session_.actions.push_back(a);
}

void SessionRecorder::record_cancel(Nanos offset_ns, const CancelRequest& request) {
    RecordedAction a;
    a.kind = RecordedAction::Kind::Cancel;
    a.offset_ns = offset_ns;
    a.cancel = request;
    session_.actions.push_back(a);
}

void SessionRecorder::record_replace(Nanos offset_ns, const ReplaceRequest& request) {
    RecordedAction a;
    a.kind = RecordedAction::Kind::Replace;
    a.offset_ns = offset_ns;
    a.replace = request;
    session_.actions.push_back(a);
}

void SessionRecorder::record_market(Nanos offset_ns, const Symbol& symbol,
                                    const BookLevels& levels, Seq update_id) {
    RecordedAction a;
    a.kind = RecordedAction::Kind::MarketUpdate;
    a.offset_ns = offset_ns;
    a.symbol = symbol;
    a.levels = levels;
    a.update_id = update_id;
    session_.actions.push_back(a);
}

void SessionRecorder::record_poll(Nanos offset_ns) {
    RecordedAction a;
    a.kind = RecordedAction::Kind::Poll;
    a.offset_ns = offset_ns;
    session_.actions.push_back(a);
}

}  // namespace mm::exchange::paper

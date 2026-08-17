#pragma once

/// \file OrderJournal.hpp
/// Append-only record of every lifecycle transition.
///
/// This is an abstraction, not a database. It exists so that durable storage
/// can be added later without touching the OMS, and so that today's tests can
/// assert on the exact sequence of transitions an event stream produced.
///
/// Records carry enough to reconstruct the transition they describe. That is
/// what makes future replay possible; no replay engine is built here.

#include <vector>

#include "mm/oms/OrderRecord.hpp"

namespace mm::oms {

class IOrderJournal {
public:
    virtual ~IOrderJournal() = default;

    IOrderJournal() = default;
    IOrderJournal(const IOrderJournal&) = delete;
    IOrderJournal& operator=(const IOrderJournal&) = delete;

    /// Called on the OMS thread for every transition. Implementations must not
    /// block: the OMS thread cannot wait on a disk (docs/concurrency.md §3).
    virtual void append(const OrderJournalRecord& record) = 0;
};

/// Discards everything. The default, so an engine without configured storage
/// still runs rather than refusing to start.
class NullOrderJournal final : public IOrderJournal {
public:
    void append(const OrderJournalRecord&) override { ++appended_; }
    [[nodiscard]] std::uint64_t appended() const noexcept { return appended_; }

private:
    std::uint64_t appended_ = 0;
};

/// Retains records in memory, bounded. For tests and for a short operator-facing
/// history; durable storage is a later phase.
///
/// When full it drops the OLDEST record and says so. Dropping the newest would
/// hide the transition that just happened, which is invariably the one being
/// investigated.
class InMemoryOrderJournal final : public IOrderJournal {
public:
    explicit InMemoryOrderJournal(std::size_t capacity = 4096) : capacity_(capacity) {
        records_.reserve(capacity);
    }

    void append(const OrderJournalRecord& record) override {
        if (records_.size() >= capacity_) {
            records_.erase(records_.begin());
            ++dropped_;
        }
        records_.push_back(record);
        ++appended_;
    }

    [[nodiscard]] const std::vector<OrderJournalRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::uint64_t appended() const noexcept { return appended_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

    /// Every record for one order, in engine-sequence order.
    [[nodiscard]] std::vector<OrderJournalRecord> for_order(LogicalOrderId id) const {
        std::vector<OrderJournalRecord> out;
        for (const auto& r : records_) {
            if (r.logical_id == id) {
                out.push_back(r);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t count_of(OrderEventType type) const {
        std::size_t n = 0;
        for (const auto& r : records_) {
            if (r.type == type) {
                ++n;
            }
        }
        return n;
    }

    void clear() {
        records_.clear();
        appended_ = 0;
        dropped_ = 0;
    }

private:
    std::vector<OrderJournalRecord> records_;
    std::size_t capacity_;
    std::uint64_t appended_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace mm::oms

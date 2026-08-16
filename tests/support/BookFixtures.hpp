#pragma once

/// Builders for normalized book events, so tests read as market data rather
/// than as struct assembly.

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "mm/exchange/common/MarketDataEvents.hpp"

namespace mm::test {

using exchange::BookLevels;
using exchange::BookSnapshotEvent;
using exchange::BookUpdateEvent;
using exchange::PriceLevel;

inline PriceLevel level(const char* price, const char* quantity) {
    PriceLevel l;
    if (!Px::parse(price, l.price)) {
        l.price = Px::zero();
    }
    if (!Qty::parse(quantity, l.quantity)) {
        l.quantity = Qty::zero();
    }
    return l;
}

using LevelSpec = std::vector<std::pair<const char*, const char*>>;

inline BookLevels levels(const LevelSpec& bids, const LevelSpec& asks) {
    BookLevels out;
    for (const auto& [px, qty] : bids) {
        out.bids[out.bid_count++] = level(px, qty);
    }
    for (const auto& [px, qty] : asks) {
        out.asks[out.ask_count++] = level(px, qty);
    }
    return out;
}

inline BookSnapshotEvent snapshot(const char* symbol, Seq last_update_id, const LevelSpec& bids,
                                  const LevelSpec& asks) {
    BookSnapshotEvent s;
    s.symbol = Symbol(symbol);
    s.last_update_id = last_update_id;
    s.levels = levels(bids, asks);
    s.is_first = true;
    s.is_last = true;
    return s;
}

inline BookUpdateEvent update(const char* symbol, Seq first_id, Seq final_id,
                              const LevelSpec& bids, const LevelSpec& asks,
                              Seq prev_final_id = kNoSeq) {
    BookUpdateEvent u;
    u.symbol = Symbol(symbol);
    u.first_update_id = first_id;
    u.final_update_id = final_id;
    u.prev_final_update_id = prev_final_id;
    u.levels = levels(bids, asks);
    u.is_last = true;
    return u;
}

}  // namespace mm::test

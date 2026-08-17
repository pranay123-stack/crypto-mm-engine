#!/usr/bin/env python3
"""Phase 3 acceptance tests B and F, enforced mechanically.

A boundary that is only enforced by review erodes one reasonable-looking include
at a time, and nobody notices until adding a second venue turns out to be a
rewrite. This script fails the build instead.

It checks two things:

  F. No venue-specific identifier appears in core code. Adapter directories
     (include/mm/exchange/<venue>/, src/exchange/<venue>/) are exempt -- that is
     precisely where venue detail belongs.

  B. No core header or source includes an adapter header, so the core compiles
     with every adapter removed from the tree.

Comments are stripped before scanning: the rule is about identifiers and types,
not about whether prose is allowed to mention a venue by name. A doc comment
reading "Binance caps client order ids at 36 chars" is exactly the kind of
context that should be written down.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Directories where venue-specific code is expected and permitted.
ADAPTER_DIRS = ("include/mm/exchange/", "src/exchange/")
# The one subdirectory of those trees that must stay venue-neutral.
NEUTRAL_ADAPTER_SUBDIR = "exchange/common/"

# Directories holding strategy code. A strategy is a pure decision function; if
# it can include a socket, a REST client or the OMS, then "the strategy cannot
# reach the exchange" is a hope rather than a property.
STRATEGY_DIRS = ("strategies/", "include/mm/strategy/", "src/strategy/")

# The quote manager translates intent into desired order state. It must not
# contain exchange code, risk logic or order management -- those are separate
# layers with separate mandates, and merging any of them here would put the
# decision "is this safe?" in the component that decides "what do we want?".
QUOTE_DIRS = ("include/mm/quote/", "src/quote/")

# Risk is the safety boundary. It must depend only on normalized abstractions:
# no venue, no order management, no strategy implementation. A risk engine that
# can reach the exchange is a risk engine that can be routed around.
RISK_DIRS = ("include/mm/risk/", "src/risk/")
RISK_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/mock/", "a venue adapter"),
    ("mm/exchange/paper/", "an execution adapter"),
    ("mm/oms/", "order management"),
    ("mm/execution/", "order execution"),
    ("strategies/", "a strategy implementation"),
    ("curl/", "networking"),
]
# The OMS owns order lifecycle state and nothing else. It must not know which
# venue it is talking to, must not contain strategy logic, and must not reach
# into risk's internals -- risk hands it an approval token and that is the
# whole of their coupling. It also has no business opening sockets: everything
# it sends goes through the execution interface it was given.
OMS_DIRS = ("include/mm/oms/", "src/oms/")
OMS_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("curl/", "networking"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/mock/", "a venue adapter"),
    ("mm/exchange/paper/", "an execution adapter"),
    ("mm/strategy/", "strategy logic"),
    ("strategies/", "a strategy implementation"),
    ("mm/risk/RiskEngine", "risk internals"),
    ("mm/risk/RiskLimits", "risk internals"),
    ("mm/dashboard/", "the dashboard"),
    ("mm/orderbook/", "market data"),
]
# Phase 8 §44. Paper and live must share one OMS; the mode lives below the
# execution interface. Deliberately narrow tokens: `is_live()` is an order's
# liveness and must not trip this.
OMS_MODE_TOKENS = re.compile(
    r"\b(is_paper|paper_mode|live_mode|is_live_mode|TradingMode|kPaperMode|kLiveMode|"
    r"PaperExchange|LiveExchange)\b"
)
# Phase 9 §36/§37. The paper adapter is an execution simulator, not a venue
# client and not part of the engine's decision-making. It may read normalized
# execution types, normalized market data and the order book; it may not reach
# a network, know a venue's wire format, or contain any strategy, risk or OMS
# logic. If it could, "paper and live differ only below the interface" would
# stop being true the moment somebody took a shortcut.
PAPER_DIRS = ("include/mm/exchange/paper/", "src/exchange/paper/")
PAPER_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("curl/", "networking"),
    ("sys/socket", "networking"),
    ("netinet/", "networking"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/mock/", "another adapter"),
    ("mm/strategy/", "strategy logic"),
    ("strategies/", "a strategy implementation"),
    ("mm/quote/", "quote decisions"),
    ("mm/risk/", "risk decisions"),
    ("mm/oms/", "order management"),
    ("mm/dashboard/", "the dashboard"),
]

# Phase 10 §"architectural boundary". Accounting consumes normalized engine
# events and nothing else. It must not reach a venue, must not become a second
# OMS, and must not know that a simulator exists.
ACCOUNTING_DIRS = ("include/mm/portfolio/", "src/portfolio/")
ACCOUNTING_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("curl/", "networking"),
    ("sys/socket", "networking"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/paper/", "the paper simulator's internals"),
    ("mm/exchange/mock/", "an adapter"),
    ("mm/oms/", "order lifecycle -- accounting is not a second OMS"),
    ("mm/orderbook/", "market data -- accounting is handed marks, it does not read a book"),
    ("mm/strategy/", "strategy logic"),
    ("strategies/", "a strategy implementation"),
    ("mm/quote/", "quote decisions"),
    ("mm/dashboard/", "the dashboard"),
]

QUOTE_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/mock/", "a venue adapter"),
    ("mm/exchange/paper/", "an execution adapter"),
    ("mm/oms/", "order management"),
    ("mm/execution/", "order execution"),
    ("mm/risk/", "risk decisions"),
    ("strategies/", "a strategy implementation"),
    ("curl/", "networking"),
]

# Headers a strategy must never pull in. Each represents a capability the
# strategy boundary exists to withhold.
STRATEGY_FORBIDDEN_INCLUDES = [
    ("boost/asio", "networking"),
    ("boost/beast", "HTTP/WebSocket"),
    ("openssl/", "TLS and signing"),
    ("nlohmann/json", "venue wire formats"),
    ("mm/exchange/binance/", "a venue adapter"),
    ("mm/exchange/mock/", "a venue adapter"),
    ("mm/exchange/paper/", "an execution adapter"),
    ("mm/oms/", "order management"),
    ("mm/execution/", "order execution"),
    ("mm/risk/", "risk state"),
    ("yaml-cpp/", "configuration parsing"),
    ("curl/", "networking"),
    ("sys/socket", "networking"),
]

VENUE_NAMES = [
    "binance", "okx", "bybit", "coinbase", "kraken",
    "hyperliquid", "deribit", "bitmex", "bitstamp", "gemini",
]

# Venue wire-protocol spellings. These are camelCase field names and headers
# that only ever come from a specific exchange's API.
VENUE_WIRE_TOKENS = [
    "listenKey", "newClientOrderId", "origClientOrderId", "recvWindow",
    "MBX-APIKEY", "X-MBX", "lastUpdateId", "asks", "bids",
    "executionReport", "outboundAccountPosition", "depthUpdate",
    "aggTrade", "bookTicker", "exchangeInfo",
]

# `asks`/`bids` are ordinary domain words, not venue tokens. Kept out of the
# scan rather than generating noise that trains people to ignore this tool.
VENUE_WIRE_TOKENS = [t for t in VENUE_WIRE_TOKENS if t not in ("asks", "bids")]


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments while preserving line count."""
    out = []
    i, n = 0, len(text)
    in_line_comment = in_block_comment = in_string = in_char = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
                out.append(c)
            i += 1
        elif in_block_comment:
            if c == "*" and nxt == "/":
                in_block_comment = False
                i += 2
            else:
                out.append(c if c == "\n" else " ")
                i += 1
        elif in_string:
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(nxt)
                i += 2
            else:
                if c == '"':
                    in_string = False
                i += 1
        elif in_char:
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(nxt)
                i += 2
            else:
                if c == "'":
                    in_char = False
                i += 1
        else:
            if c == "/" and nxt == "/":
                in_line_comment = True
                i += 2
            elif c == "/" and nxt == "*":
                in_block_comment = True
                i += 2
            else:
                if c == '"':
                    in_string = True
                elif c == "'":
                    in_char = True
                out.append(c)
                i += 1
    return "".join(out)


def is_strategy_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in STRATEGY_DIRS)


def is_quote_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in QUOTE_DIRS)


def SUPPORT_DIRS_GLOB():
    """Shared test fixture headers, which are included together and so share a
    namespace across translation units."""
    return (ROOT / "tests" / "support").glob("*.hpp")


def is_accounting_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in ACCOUNTING_DIRS)


def is_paper_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in PAPER_DIRS)


def is_oms_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in OMS_DIRS)


def is_risk_file(rel: str) -> bool:
    return any(rel.startswith(d) for d in RISK_DIRS)


def is_adapter_file(rel: str) -> bool:
    """True for files inside a venue adapter directory (not exchange/common)."""
    if NEUTRAL_ADAPTER_SUBDIR in rel:
        return False
    return any(rel.startswith(d) for d in ADAPTER_DIRS)


def scan() -> int:
    violations = []
    scanned = 0

    sources = sorted(
        list((ROOT / "include").rglob("*.hpp"))
        + list((ROOT / "src").rglob("*.cpp"))
        + list((ROOT / "src").rglob("*.hpp"))
        + list((ROOT / "strategies").rglob("*.hpp"))
        + list((ROOT / "strategies").rglob("*.cpp"))
    )

    # Boundary required BEFORE the name but deliberately NOT after. Requiring a
    # trailing boundary silently missed the most likely leak form there is --
    # a venue name embedded in an identifier, as in `BinanceDepthUpdate` or
    # `parseBinanceMessage`. That gap was found by the self-test below.
    venue_pattern = re.compile(
        r"\b(" + "|".join(re.escape(v) for v in VENUE_NAMES) + r")", re.IGNORECASE
    )
    wire_pattern = re.compile("|".join(re.escape(t) for t in VENUE_WIRE_TOKENS))
    adapter_include = re.compile(
        r'#\s*include\s*[<"]mm/exchange/(?!common/)([A-Za-z0-9_]+)/'
    )

    for path in sources:
        rel = str(path.relative_to(ROOT))
        scanned += 1
        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)

        # --- Test B: core must not include an adapter header -------------
        for lineno, line in enumerate(code.splitlines(), 1):
            m = adapter_include.search(line)
            if m and not is_adapter_file(rel):
                violations.append(
                    f"{rel}:{lineno}: core file includes adapter header "
                    f"'mm/exchange/{m.group(1)}/...' - the core must compile with "
                    f"every adapter removed"
                )

        # --- Strategy layer: no capability it is meant to be denied --------
        if is_strategy_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in STRATEGY_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: strategy code includes '{needle}' "
                            f"({capability}); the strategy boundary exists to withhold it"
                        )

        # --- Quote manager: no exchange, risk or OMS code ------------------
        if is_quote_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in QUOTE_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: quote manager includes '{needle}' "
                            f"({capability}); that decision belongs to another layer"
                        )

        # --- Risk: no venue, order management or strategy code -------------
        if is_risk_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in RISK_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: risk engine includes '{needle}' "
                            f"({capability}); the safety boundary must not reach it"
                        )

        # --- Test E2: OMS capability denial (Phase 8 §37) ----------------
        if is_oms_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in OMS_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: OMS includes '{needle}' "
                            f"({capability}); the OMS owns order state and nothing else"
                        )

        # --- Test E3: no paper/live branching inside the OMS (§44) -------
        if is_oms_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                m = OMS_MODE_TOKENS.search(line)
                if m is not None:
                    violations.append(
                        f"{rel}:{lineno}: OMS references '{m.group(1)}'; paper and live "
                        f"must differ only below the execution interface"
                    )

        # --- Test E4: paper adapter capability denial (Phase 9 §36) ------
        if is_paper_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in PAPER_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: paper execution includes '{needle}' "
                            f"({capability}); it simulates a venue, it is not one"
                        )

        # --- Test E5: accounting capability denial (Phase 10) ------------
        if is_accounting_file(rel):
            for lineno, line in enumerate(code.splitlines(), 1):
                stripped = line.strip()
                if not stripped.startswith("#include"):
                    continue
                for needle, capability in ACCOUNTING_FORBIDDEN_INCLUDES:
                    if needle in stripped:
                        violations.append(
                            f"{rel}:{lineno}: accounting includes '{needle}' "
                            f"({capability}); it consumes normalized events and owns only "
                            f"position, cost basis, PnL, fees and balances"
                        )

        # --- Test F: no venue identifiers outside adapter directories ----
        if is_adapter_file(rel):
            continue
        for lineno, line in enumerate(code.splitlines(), 1):
            for m in venue_pattern.finditer(line):
                violations.append(
                    f"{rel}:{lineno}: venue name '{m.group(1)}' in core code: "
                    f"{line.strip()[:90]}"
                )
            for m in wire_pattern.finditer(line):
                violations.append(
                    f"{rel}:{lineno}: venue wire token '{m.group(0)}' in core code: "
                    f"{line.strip()[:90]}"
                )

    # --- Test G: no duplicate type name across the shared test fixtures ------
    #
    # Two classes with the same name in the same namespace are an ODR violation
    # even when both are `final` and in different headers: the linker merges
    # their inline functions and may run one class's destructor over the other's
    # object. That produced a heap-use-after-free in Phase 9 that only ASan
    # caught, so it is checked mechanically here rather than trusted to review.
    fixture_types: dict[str, str] = {}
    type_decl = re.compile(r"^\s*(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:final)?\s*[:{]")
    for path in sorted(SUPPORT_DIRS_GLOB()):
        rel = str(path.relative_to(ROOT))
        try:
            code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        for lineno, line in enumerate(code.splitlines(), 1):
            m = type_decl.match(line)
            if m is None:
                continue
            name = m.group(1)
            if name in fixture_types and fixture_types[name] != rel:
                violations.append(
                    f"{rel}:{lineno}: '{name}' is also defined in {fixture_types[name]}; "
                    f"two types of the same name in one namespace is an ODR violation"
                )
            else:
                fixture_types[name] = rel

    print(f"exchange boundary check: scanned {scanned} files")
    if violations:
        print(f"\nFAILED with {len(violations)} violation(s):\n")
        for v in violations:
            print(f"  {v}")
        return 1
    print("OK: no venue-specific identifiers or adapter includes in core code")
    return 0


def self_test() -> int:
    """Verifies the checker actually detects the leaks it claims to.

    A boundary checker that silently passes everything is worse than none: it
    reports success and trains people to trust it. These cases are the forms a
    real leak takes.
    """
    venue_pattern = re.compile(
        r"\b(" + "|".join(re.escape(v) for v in VENUE_NAMES) + r")", re.IGNORECASE
    )
    adapter_include = re.compile(
        r'#\s*include\s*[<"]mm/exchange/(?!common/)([A-Za-z0-9_]+)/'
    )

    must_detect = [
        "struct BinanceDepthUpdate { int u; };",      # embedded in CamelCase
        "using binance_seq = uint64_t;",              # snake_case prefix
        "static const char* BINANCE_HOST = \"x\";",   # SCREAMING_CASE
        "void parse(const OkxBook& b);",              # a different venue
        "class BybitFeed;",
    ]
    must_ignore = [
        "struct DepthUpdate { int u; };",
        "using venue_seq = uint64_t;",
        "int finance_rate = 0;",                      # 'finance' must not match
    ]
    include_must_detect = ['#include "mm/exchange/binance/BinanceCodec.hpp"']
    include_must_ignore = ['#include "mm/exchange/common/ExchangeTypes.hpp"']

    # Strategy-layer capability denial.
    strategy_must_detect = [
        '#include <boost/asio/io_context.hpp>',
        '#include <boost/beast/websocket.hpp>',
        '#include <openssl/hmac.h>',
        '#include <nlohmann/json.hpp>',
        '#include "mm/exchange/binance/BinanceMarketData.hpp"',
        '#include "mm/oms/OrderStore.hpp"',
        '#include "mm/risk/RiskEngine.hpp"',
    ]
    quote_must_detect = [
        '#include "mm/risk/RiskEngine.hpp"',
        '#include "mm/oms/OrderStore.hpp"',
        '#include "mm/exchange/binance/BinanceMarketData.hpp"',
        '#include <boost/beast/websocket.hpp>',
        '#include "strategies/reference_mm_v1/ReferenceMarketMaker.hpp"',
    ]
    quote_must_ignore = [
        '#include "mm/strategy/QuoteIntent.hpp"',
        '#include "mm/exchange/common/OrderRequest.hpp"',
        '#include "mm/exchange/common/ExchangeCapabilities.hpp"',
        '#include "mm/common/Time.hpp"',
    ]

    risk_must_detect = [
        '#include "mm/exchange/binance/BinanceMarketData.hpp"',
        '#include "mm/oms/OrderStore.hpp"',
        '#include <boost/asio/io_context.hpp>',
        '#include "strategies/reference_mm_v1/ReferenceMarketMaker.hpp"',
    ]
    oms_must_detect = [
        '#include "mm/exchange/binance/BinanceExecution.hpp"',
        '#include <boost/beast/websocket.hpp>',
        '#include <nlohmann/json.hpp>',
        '#include "strategies/reference_mm_v1/ReferenceMarketMaker.hpp"',
        '#include "mm/strategy/StrategyRuntime.hpp"',
        '#include "mm/risk/RiskEngine.hpp"',
        '#include "mm/orderbook/OrderBook.hpp"',
    ]
    oms_must_ignore = [
        # The approval token is the entire risk-to-OMS interface, and the OMS
        # must be able to name the types it publishes for risk and the quote
        # manager to read.
        '#include "mm/risk/RiskDecision.hpp"',
        '#include "mm/risk/Exposure.hpp"',
        '#include "mm/quote/WorkingQuoteState.hpp"',
        '#include "mm/exchange/common/ExecutionEvents.hpp"',
        '#include "mm/exchange/IExchangeExecution.hpp"',
        '#include "mm/common/Fixed.hpp"',
    ]

    risk_must_ignore = [
        '#include "mm/quote/OrderAction.hpp"',
        '#include "mm/exchange/common/OrderRequest.hpp"',
        '#include "mm/common/Fixed.hpp"',
    ]

    strategy_must_ignore = [
        '#include "mm/strategy/IStrategy.hpp"',
        '#include "mm/common/Types.hpp"',
        '#include "mm/exchange/common/MarketDataEvents.hpp"',
        '#include "mm/orderbook/OrderBook.hpp"',
    ]

    failures = []
    for line in must_detect:
        if not venue_pattern.search(strip_comments(line)):
            failures.append(f"missed a venue identifier: {line}")
    for line in must_ignore:
        if venue_pattern.search(strip_comments(line)):
            failures.append(f"false positive on: {line}")
    for line in include_must_detect:
        if not adapter_include.search(line):
            failures.append(f"missed an adapter include: {line}")
    for line in include_must_ignore:
        if adapter_include.search(line):
            failures.append(f"false positive on include: {line}")

    # Comments mention venues legitimately and must never trip the check.
    if venue_pattern.search(strip_comments("// Binance caps client order ids at 36 chars")):
        failures.append("a comment mentioning a venue was treated as a leak")
    if venue_pattern.search(strip_comments("/* uses the Binance depth stream */ int x;")):
        failures.append("a block comment mentioning a venue was treated as a leak")

    def forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in STRATEGY_FORBIDDEN_INCLUDES)

    def quote_forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in QUOTE_FORBIDDEN_INCLUDES)

    for line in strategy_must_detect:
        if not forbidden_hit(line):
            failures.append(f"missed a forbidden strategy include: {line}")
    for line in strategy_must_ignore:
        if forbidden_hit(line):
            failures.append(f"false positive on a permitted strategy include: {line}")
    def risk_forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in RISK_FORBIDDEN_INCLUDES)

    for line in quote_must_detect:
        if not quote_forbidden_hit(line):
            failures.append(f"missed a forbidden quote-manager include: {line}")
    for line in quote_must_ignore:
        if quote_forbidden_hit(line):
            failures.append(f"false positive on a permitted quote include: {line}")
    for line in risk_must_detect:
        if not risk_forbidden_hit(line):
            failures.append(f"missed a forbidden risk include: {line}")
    for line in risk_must_ignore:
        if risk_forbidden_hit(line):
            failures.append(f"false positive on a permitted risk include: {line}")

    mode_must_detect = [
        "if (is_paper) { skip_venue(); }",
        "if (config.live_mode) { sign(request); }",
        "TradingMode mode = TradingMode::Paper;",
        "PaperExchange* venue = nullptr;",
    ]
    mode_must_ignore = [
        # These are the OMS's own vocabulary and must never trip the check.
        "if (order.is_live()) { ++live; }",
        "[[nodiscard]] bool is_live(OrderState s) noexcept;",
        "std::size_t live_order_count() const;",
        "// paper and live share this code path",
    ]
    for line in mode_must_detect:
        if OMS_MODE_TOKENS.search(line) is None:
            failures.append(f"missed a paper/live branch in the OMS: {line}")
    for line in mode_must_ignore:
        if OMS_MODE_TOKENS.search(line) is not None:
            failures.append(f"false positive on OMS liveness vocabulary: {line}")

    paper_must_detect = [
        '#include <boost/asio/io_context.hpp>',
        '#include <boost/beast/websocket.hpp>',
        '#include <nlohmann/json.hpp>',
        '#include <sys/socket.h>',
        '#include "mm/exchange/binance/BinanceExecution.hpp"',
        '#include "mm/oms/OrderManager.hpp"',
        '#include "mm/risk/RiskEngine.hpp"',
        '#include "mm/strategy/IStrategy.hpp"',
    ]
    paper_must_ignore = [
        # Exactly what a venue simulator legitimately needs: the normalized
        # interface it implements, the normalized market data it reads, and the
        # engine's own book.
        '#include "mm/exchange/common/IExchangeExecution.hpp"',
        '#include "mm/exchange/common/ExecutionEvents.hpp"',
        '#include "mm/exchange/common/MarketDataEvents.hpp"',
        '#include "mm/orderbook/OrderBook.hpp"',
        '#include "mm/common/Fixed.hpp"',
    ]

    def paper_forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in PAPER_FORBIDDEN_INCLUDES)

    for line in paper_must_detect:
        if not paper_forbidden_hit(line):
            failures.append(f"missed a forbidden paper-execution include: {line}")
    for line in paper_must_ignore:
        if paper_forbidden_hit(line):
            failures.append(f"false positive on a permitted paper include: {line}")

    accounting_must_detect = [
        '#include <boost/asio/io_context.hpp>',
        '#include <nlohmann/json.hpp>',
        '#include "mm/exchange/binance/BinanceExecution.hpp"',
        '#include "mm/exchange/paper/PaperExecution.hpp"',
        '#include "mm/oms/OrderManager.hpp"',
        '#include "mm/orderbook/OrderBook.hpp"',
        '#include "mm/strategy/IStrategy.hpp"',
        '#include "mm/quote/QuoteManager.hpp"',
    ]
    accounting_must_ignore = [
        # Normalized execution events, the risk types it publishes, and the
        # engine's own arithmetic. Exactly what accounting legitimately needs.
        '#include "mm/exchange/common/ExecutionEvents.hpp"',
        '#include "mm/risk/PositionState.hpp"',
        '#include "mm/risk/PortfolioState.hpp"',
        '#include "mm/common/Fixed.hpp"',
        '#include "mm/common/Time.hpp"',
    ]

    def accounting_forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in ACCOUNTING_FORBIDDEN_INCLUDES)

    for line in accounting_must_detect:
        if not accounting_forbidden_hit(line):
            failures.append(f"missed a forbidden accounting include: {line}")
    for line in accounting_must_ignore:
        if accounting_forbidden_hit(line):
            failures.append(f"false positive on a permitted accounting include: {line}")

    def oms_forbidden_hit(line: str) -> bool:
        stripped = line.strip()
        if not stripped.startswith("#include"):
            return False
        return any(needle in stripped for needle, _ in OMS_FORBIDDEN_INCLUDES)

    for line in oms_must_detect:
        if not oms_forbidden_hit(line):
            failures.append(f"missed a forbidden OMS include: {line}")
    for line in oms_must_ignore:
        if oms_forbidden_hit(line):
            failures.append(f"false positive on a permitted OMS include: {line}")

    if failures:
        print("SELF-TEST FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    detections = (len(must_detect) + len(strategy_must_detect) + len(quote_must_detect) +
                  len(risk_must_detect) + len(oms_must_detect) + len(mode_must_detect) +
                  len(paper_must_detect) + len(accounting_must_detect))
    non_detections = (len(must_ignore) + len(strategy_must_ignore) + len(quote_must_ignore) +
                      len(risk_must_ignore) + len(oms_must_ignore) + len(mode_must_ignore) +
                      len(paper_must_ignore) + len(accounting_must_ignore))
    print(f"self-test: {detections} detections and {non_detections} non-detections "
          f"behave as expected")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(self_test() or scan())

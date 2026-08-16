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
    for line in quote_must_detect:
        if not quote_forbidden_hit(line):
            failures.append(f"missed a forbidden quote-manager include: {line}")
    for line in quote_must_ignore:
        if quote_forbidden_hit(line):
            failures.append(f"false positive on a permitted quote include: {line}")

    if failures:
        print("SELF-TEST FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"self-test: {len(must_detect) + len(strategy_must_detect) + len(quote_must_detect)} "
          f"detections and "
          f"{len(must_ignore) + len(strategy_must_ignore) + len(quote_must_ignore)} "
          f"non-detections behave as expected")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(self_test() or scan())

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
    )

    venue_pattern = re.compile(
        r"\b(" + "|".join(re.escape(v) for v in VENUE_NAMES) + r")\b", re.IGNORECASE
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


if __name__ == "__main__":
    sys.exit(scan())

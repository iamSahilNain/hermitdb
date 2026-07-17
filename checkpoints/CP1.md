# CP1 — Incremental RESP2 Parser

**Files:** `src/protocol/resp_parser.{h,cpp}` · **Milestone:** M1
**Test:** `make build && ctest --test-dir build -L cp1 --output-on-failure`

## Contract

`RespParser::feed(std::string_view)` consumes an arbitrary byte chunk and
returns zero or more *complete* commands plus a status. TCP hands you a byte
stream, not messages: one command may arrive over ten `recv()`s, or fifty
commands in one. Partial trailing input persists inside the parser between
calls. Two framings: array-of-bulk-strings (`*n\r\n` then `$len\r\n<bytes>\r\n`
each) and inline (`PING\r\n`, whitespace-split — redis-cli sends these). Bulk
payloads are binary-safe: `\r\n` inside a payload is data, not a terminator.
Any declared or accumulated size beyond `max_frame_bytes` ⇒ `kProtocolError`
*before* allocating. After `kProtocolError` the connection closes; your state
may be left in any condition.

## Guiding questions (work these out before coding)

1. What is the minimal set of states your parser can be "resting" in between
   two feed() calls? Enumerate them before choosing a data structure.
2. When a bulk length header says `$5` but only 3 payload bytes have arrived,
   what exactly do you store, and what do you re-examine on the next feed()?
3. Buffer strategy: one growing buffer you re-scan, or a cursor you never move
   backwards? What does each cost when a 1 MiB value arrives one byte at a time?
4. How do you distinguish "inline command" from "array command" from "garbage"
   using only the first byte — and when is that decision made?
5. Where exactly is the earliest point you can reject `*1000000000\r\n`?

## Interview questions you must be able to answer (SPEC §5)

- How do you know a message is complete?
- Why can't you `recv()` a "whole command"?
- State machine vs recursive descent — why here?
- What's the memory blow-up attack (`*1000000000\r\n`) and your defense?

## Edge cases (the test suite probes all of these)

- Command split at *every* byte offset, incl. mid-`\r\n` and mid-length-digit.
- Multiple commands in one chunk; commands interleaved across framings.
- `$0\r\n\r\n` (empty argument) and payloads containing `\r\n`.
- `*0\r\n` and a bare `\r\n` inline line: legal, produce nothing.
- Non-numeric / negative lengths, `:` where `$` is required, missing CRLF
  after a payload.
- Oversized array header, oversized bulk header, unterminated inline line
  growing past the cap — all must error without allocating the claimed size.
- Unresolved by the tests, decide and document in the header: what should
  `*-1\r\n` from a *client* do? (Check what real Redis does.)

# CP2 — epoll Event Loop

**Files:** `src/net/event_loop.{h,cpp}` · **Milestone:** M2
**Decide first:** DECISION-1 (LT vs ET) in DECISIONS.md.
**Test:** `ctest --test-dir build -L cp2 --output-on-failure`
**Manual gate:** `make run`, then `make cli` → `PING` answers `PONG`.

## Contract

Single-threaded reactor. `run()` blocks on `epoll_wait` and dispatches:
listener readable → `accept_client()` in a loop until EAGAIN, register the new
`Connection` (respect `cfg.max_clients`); connection readable → drain `recv`,
feed `conn.parser()`, execute each complete command via the handler, queue the
reply; writable → flush `conn.outbuf()`. Short write or EAGAIN on send ⇒ park
the remainder, register EPOLLOUT, deregister once drained — a slow reader must
never stall other clients. `recv()==0` / ECONNRESET ⇒ destroy the connection.
Protocol error ⇒ queue `-ERR ...`, flush, close. Tick callbacks (`add_tick`)
fire ~every interval from inside the loop (they drive CP3 active expiry and,
later, CP4 everysec fsync). `stop()` may be called from inside a handler.
Plumbing you get for free: `Listener` (nonblocking accept, TCP_NODELAY),
`Connection` (fd + parser + outbuf, closes on destruction).

## Guiding questions

1. What precisely does `epoll_wait` returning "readable" promise you — and
   what does it NOT promise about the *next* `recv()`?
2. Under your DECISION-1 choice, when is it safe to stop reading a connection
   that still has buffered bytes in the kernel?
3. Why must EPOLLOUT be registered lazily and deregistered eagerly? What
   happens to your CPU if you leave it registered on an idle connection?
4. In what order do you handle a connection that is simultaneously readable,
   writable, and marked close-after-flush?
5. A handler calls stop() mid-dispatch with 30 events still in this batch —
   what do you do with them?

## Interview questions you must be able to answer (SPEC §5)

- epoll vs select/poll — what's actually O(1)?
- LT vs ET semantics and the ET starvation bug.
- Why nonblocking sockets are mandatory with ET.
- What happens when `send()` returns a short write?
- Thundering herd.
- C10K in one paragraph.

## Edge cases

- 200 clients pipelining at once (test); accept burst deeper than one
  epoll_wait round.
- Slow reader that never drains — bounded memory or bounded patience, but
  never a stalled loop (test).
- RST mid-command (test); EPOLLHUP/EPOLLERR without a prior read event.
- fd numbers are reused by the kernel immediately after close — why does that
  make "map from fd" bookkeeping dangerous if you defer erasure?
- EINTR from epoll_wait.

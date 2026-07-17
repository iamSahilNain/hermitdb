# CP4 — Write-Ahead Log

**Files:** `src/persist/wal.{h,cpp}` · **Milestone:** M5
**Decide first:** DECISION-2 (fsync default; document the append-vs-reply
ordering analysis under it).
**Test:** `ctest --test-dir build -L cp4 --output-on-failure`
(unit round-trip + torn-tail, then the kill -9 harness in
`tests/integration/test_kill9_recovery.py`)

## Contract

`append(cmd)` serializes each mutating command in RESP array encoding (reuse
`resp_writer`; the WAL format IS the wire format, so `replay` can reuse your
CP1 parser) to an append-only file. fsync per policy: `always` = per append;
`everysec` = `tick_fsync()` from the loop tick; `no` = kernel's choice.
Whether the append happens before or after the client reply is YOUR trade-off
— reason it out in DECISIONS.md. `replay(fn)` streams commands from the start;
a torn FINAL record (crash mid-append) is normal: stop, truncate, `kOk`.
Damage earlier is `kCorrupt`. `rewrite(write_snapshot)` compacts: snapshot to
a temp file (serializer provided), fsync it, `rename()` over the target,
fsync the DIRECTORY, truncate the WAL — you own that ordering and the
crash-window analysis at every step. Boot: load snapshot, replay tail.

## Guiding questions

1. Write the full data path of one `SET`: user buffer → libc → page cache →
   disk. Which arrow does each of `write()`, `fsync()`, `fdatasync()` move
   data across?
2. For each fsync policy, at what exact moment can the server truthfully tell
   the client "your write is durable"? Draw the crash line.
3. Why fsync the *directory* after `rename()`? What survives a crash if you
   don't?
4. How do you detect a torn final record given RESP framing — and how do you
   distinguish it from corruption in the middle of the file?
5. During `rewrite()`, writes keep arriving. Where do they go so that neither
   the old nor the new log loses them?

## Interview questions you must be able to answer (SPEC §5)

- What does `write()` returning actually guarantee?
- Page cache vs disk; what `fsync` costs (measure it — `bench/run_bench.sh`
  emits µs/fsync; the number goes in the README table).
- Crash between WAL append and reply — is the client's ACKed write durable
  under each fsync policy?
- Why `rename()` is the atomic primitive.
- Group commit — how would you batch fsyncs?

## Edge cases

- Crash mid-append (torn tail — tolerated, test), crash between snapshot
  rename and WAL truncate (must not double-apply or lose), crash mid-rewrite
  (old snapshot+log must still recover).
- Replay of a `DEL` logged by expiry (CP3 contract): never re-evaluate TTLs
  against the replaying clock.
- Disk full on append: what does the client hear?
- Empty file vs absent file on boot (both fine, test).
- fsync=everysec: tick fires while an append is mid-write? (Single-threaded
  loop — does that ordering even arise? Know why.)

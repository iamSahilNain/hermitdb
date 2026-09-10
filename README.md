# HermitDB

A C++17 in-memory key-value server with a RESP2 command subset, Linux epoll,
TTL expiry, and append-only persistence. It works with Redis protocol clients;
it is a learning project, not a drop-in replacement for Redis.

## Implementation and attribution

Claude Code generated the initial scaffolding and CP1–CP5 implementations.
This revision also includes AI-assisted review and repairs. The original
`scaffold:` and `ai-cp` commits remain in history. The repository does not claim
that these components were handwritten. Original learning materials remain in
[SPEC_kvstore.md](SPEC_kvstore.md), [HANDOFF.md](HANDOFF.md), and
[checkpoints/](checkpoints/).

## Run

```sh
docker build -t hermitdb .
docker run --rm -p 6380:6380 hermitdb
redis-cli -p 6380 SET greeting hello EX 60
redis-cli -p 6380 GET greeting
```

To retain the WAL across container restarts:

```sh
docker run --rm -p 6380:6380 -v hermitdata:/data hermitdb \
  --port=6380 --data-dir=/data --wal --fsync=always
```

`--wal` requires an explicit policy: `always`, `everysec`, or `no`.
`always` synchronizes log records before successful replies. `everysec`
attempts synchronization from an event-loop tick; scheduling and I/O delays
mean this is not a strict one-second loss bound. `no` relies on OS flushing.
A process kill is not a power-loss test.

## Commands

| Area | Supported commands |
|---|---|
| Connection | PING, ECHO, COMMAND, SHUTDOWN |
| Strings | SET (EX/PX/NX/XX), GET, INCR, DECR, INCRBY |
| Keys | DEL, EXISTS, TYPE, KEYS, DBSIZE, FLUSHALL |
| Expiry | EXPIRE, PEXPIRE, PEXPIREAT, TTL, PTTL, PERSIST |
| Lists | LPUSH, RPUSH, LPOP, RPOP, LRANGE, LLEN |
| Configuration | CONFIG GET (stub) |

## Architecture

Each reactor owns its accepted connections and performs nonblocking reads,
incremental parsing, and buffered writes. With `--threads=N`, reactors share
one keyspace. A mutex covers the whole command dispatcher, including reads,
reply construction, and WAL work. Socket I/O and parsing happen outside it.
This design prioritizes simple serialization over parallel command execution.

Expiry combines checks on access with a sampled background pass. The WAL
stores RESP commands and translates relative expiry into absolute deadlines.
Startup can load an existing snapshot and replay the log.

Detected WAL write or sync failures put the dispatcher into a failed state
and stop serving. The failed operation may already have changed memory; this
is fail-closed handling, not transactional rollback, and clients may see an
error or disconnect. Recovery can include an operation whose reply was lost.

## Build and test

Development requires Linux; Docker supplies GCC and epoll on macOS hosts.

```sh
make image
make configure
make test
```

`make test` runs the complete suite. CI also exercises integration tests with
1, 2, 4, and 8 reactors and runs threaded integration under ThreadSanitizer.
See [docs/verification.md](docs/verification.md) for checks actually executed
on this revision and their limits. A workflow definition is not a recorded CI pass.

## Benchmarks

`bench/run_bench.sh` generates a Redis comparison matrix and raw CSV files.
The old published tables are retained in the
[historical README](docs/history/README.md); their raw runs were not committed
and have not been independently reproduced in this repair pass.

Do not infer a one-million-key workload from one million requests. The current
script does not run three-trial medians or select a 100-byte payload.
New performance claims need the exact command,
keyspace, payload, software versions, machine details, commit, and raw outputs.

## Limitations

- No authentication, TLS, replication, or per-user isolation. Keep it on a trusted network.
- No maxmemory or eviction policy. The keyspace can exhaust memory.
- Snapshot compaction is disabled: the old rename/truncate sequence could
  replay non-idempotent operations twice after a crash. `Wal::rewrite()` returns
  `kNotImplemented` without touching files. No runtime command invokes it.
- WAL records have no checksum. Truncated-tail handling does not detect all corruption.
- Snapshot loading, TTL replay, filesystem durability, and protocol compatibility
  have narrower coverage than a production database requires.

[Design notes](DECISIONS.md) explain the current trade-offs.

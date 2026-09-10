# Design notes

These notes describe the current implementation. They are AI-assisted and do
not represent handwritten decisions by the repository owner. The original
[decision log](docs/history/DECISIONS.md) is retained for provenance.

## Reactor and locking

Level-triggered epoll allows the read loop to stop at its fairness budget and
receive readiness again. One shared keyspace mutex serializes entire commands,
including response encoding and WAL synchronization. More reactor threads do
not guarantee more throughput; no new scaling measurement is claimed here.

## Persistence errors

Commands mutate memory before logging. A failed append therefore cannot be
handled by merely reporting success or continuing to serve that state. The
dispatcher latches a persistence failure, refuses subsequent commands, and the
runtime stops. This does not roll back the mutation or establish whether an
unacknowledged operation reached storage.

## Recovery and compaction

Absolute expiry timestamps avoid restarting relative TTL durations on replay.
Existing snapshots can be loaded before the WAL. Correctness across arbitrary
TTL-dependent histories still requires additional testing.

The old compaction sequence installed a snapshot before truncating the WAL.
A crash between those operations could replay INCR or list mutations on top of
state that already contained them. Compaction now refuses to run. A future
implementation needs a durable snapshot/log generation or replay offset, with
crash tests at every file transition; an atomic rename alone is insufficient.

## Containers and bounds

The keyspace uses std::unordered_map; no profiling claim supports replacing it.
Network caps are partial resource defenses, not a global memory budget. There
is no authentication or eviction. This server is for controlled experiments.

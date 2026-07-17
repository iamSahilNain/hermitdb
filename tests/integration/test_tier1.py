#!/usr/bin/env python3
"""M3 gate: Tier 1 command set over a real socket. Green after Claude Code
wires db + commands (which happens AFTER the human's M1/M2, per SPEC §4)."""
from hermit_test_lib import Client, RespError, Server, binary_from_argv, expect

with Server(binary_from_argv()) as srv:
    c = Client(srv.port)

    # strings
    expect(c.cmd("SET", "k", "v") == "OK", "SET -> +OK")
    expect(c.cmd("GET", "k") == b"v", "GET returns value")
    expect(c.cmd("GET", "missing") is None, "GET missing -> nil")
    expect(c.cmd("EXISTS", "k") == 1, "EXISTS hit -> :1")
    expect(c.cmd("EXISTS", "missing") == 0, "EXISTS miss -> :0")
    expect(c.cmd("DEL", "k") == 1, "DEL -> :1")
    expect(c.cmd("GET", "k") is None, "deleted key is gone")

    # SET options
    expect(c.cmd("SET", "nx", "1", "NX") == "OK", "SET NX on absent key")
    expect(c.cmd("SET", "nx", "2", "NX") is None, "SET NX on present key -> nil")
    expect(c.cmd("SET", "nx", "3", "XX") == "OK", "SET XX on present key")
    expect(c.cmd("SET", "xx-absent", "1", "XX") is None, "SET XX on absent key -> nil")
    expect(c.cmd("SET", "tmp", "v", "EX", "100") == "OK", "SET EX")
    ttl = c.cmd("TTL", "tmp")
    expect(isinstance(ttl, int) and 90 <= ttl <= 100, f"TTL in range, got {ttl}")
    pttl = c.cmd("PTTL", "tmp")
    expect(isinstance(pttl, int) and 90_000 <= pttl <= 100_000, f"PTTL in range, got {pttl}")
    expect(c.cmd("PERSIST", "tmp") == 1, "PERSIST -> :1")
    expect(c.cmd("TTL", "tmp") == -1, "TTL after PERSIST -> :-1")
    expect(c.cmd("TTL", "missing") == -2, "TTL of missing key -> :-2")

    # counters
    expect(c.cmd("INCR", "n") == 1, "INCR fresh -> :1")
    expect(c.cmd("INCR", "n") == 2, "INCR -> :2")
    expect(c.cmd("INCRBY", "n", "40") == 42, "INCRBY -> :42")
    expect(c.cmd("DECR", "n") == 41, "DECR -> :41")
    c.cmd("SET", "s", "notanumber")
    r = c.cmd("INCR", "s")
    expect(isinstance(r, RespError) and "not an integer" in r.message,
           f"INCR on non-numeric errors, got {r!r}")

    # keyspace
    expect(c.cmd("TYPE", "s") == "string", "TYPE -> +string")
    expect(c.cmd("TYPE", "missing") == "none", "TYPE missing -> +none")
    size = c.cmd("DBSIZE")
    expect(isinstance(size, int) and size > 0, "DBSIZE > 0")
    c.cmd("SET", "glob:1", "a")
    c.cmd("SET", "glob:2", "b")
    keys = c.cmd("KEYS", "glob:*")
    expect(sorted(keys) == [b"glob:1", b"glob:2"], f"KEYS glob, got {keys}")
    expect(c.cmd("FLUSHALL") == "OK", "FLUSHALL -> +OK")
    expect(c.cmd("DBSIZE") == 0, "DBSIZE after FLUSHALL -> :0")

    # error semantics
    r = c.cmd("GET")
    expect(isinstance(r, RespError) and
           r.message == "ERR wrong number of arguments for 'get' command",
           f"arity error format, got {r!r}")

    # CONFIG GET stub is allowed, but must not break redis-benchmark
    r = c.cmd("CONFIG", "GET", "save")
    expect(isinstance(r, list), "CONFIG GET returns an array (stub ok)")
    c.close()
print("OK")

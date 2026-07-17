#!/usr/bin/env python3
"""CP2: 200 concurrent clients, each pipelining a batch of commands.
Exercises accept bursts, per-connection parser state, and reply ordering."""
import threading

from hermit_test_lib import Client, Server, binary_from_argv, encode, expect

N_CLIENTS = 200
PIPELINE = 50

failures = []


def worker(port, cid):
    try:
        c = Client(port)
        batch = b"".join(encode("PING", f"c{cid}:{i}") for i in range(PIPELINE))
        c.send_raw(batch)
        for i in range(PIPELINE):
            got = c.read_reply()
            want = f"c{cid}:{i}".encode()
            if got != want:
                failures.append(f"client {cid} reply {i}: want {want!r} got {got!r}")
                return
        c.close()
    except Exception as e:  # noqa: BLE001 — report, don't hang the test
        failures.append(f"client {cid}: {e!r}")


with Server(binary_from_argv()) as srv:
    threads = [threading.Thread(target=worker, args=(srv.port, i)) for i in range(N_CLIENTS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=60)
    expect(not failures, "; ".join(failures[:5]))
print("OK")

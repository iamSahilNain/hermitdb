#!/usr/bin/env python3
"""CP5: the canonical lost-update detector. N clients x M INCRs on one key
must end at exactly N*M — under whatever threading model DECISION-4 picked.
(Also meaningful pre-CP5: a correct single-threaded server passes trivially.)"""
import threading

from hermit_test_lib import Client, Server, binary_from_argv, encode, expect

N_CLIENTS = 16
M_INCRS = 500

failures = []


def worker(port):
    try:
        c = Client(port)
        # Pipeline in batches of 50 to actually stress interleaving.
        for start in range(0, M_INCRS, 50):
            n = min(50, M_INCRS - start)
            c.send_raw(encode("INCR", "counter") * n)
            for _ in range(n):
                r = c.read_reply()
                if not isinstance(r, int):
                    failures.append(f"INCR returned {r!r}")
                    return
        c.close()
    except Exception as e:  # noqa: BLE001
        failures.append(repr(e))


with Server(binary_from_argv()) as srv:
    threads = [threading.Thread(target=worker, args=(srv.port,)) for _ in range(N_CLIENTS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=120)
    expect(not failures, "; ".join(failures[:5]))

    c = Client(srv.port)
    final = c.cmd("GET", "counter")
    want = str(N_CLIENTS * M_INCRS).encode()
    expect(final == want, f"lost updates: want {want!r}, got {final!r}")
    c.close()
print("OK")

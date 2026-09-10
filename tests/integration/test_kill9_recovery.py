#!/usr/bin/env python3
"""Process-crash recovery checks (SIGKILL, not power-loss simulation).

With fsync=always every acknowledged operation must survive. A final command
sent without reading its reply may be present or absent. Random kill delays
exercise that race; they do not guarantee a kill inside a particular syscall.
The everysec check verifies prefix consistency only: SIGKILL retains the OS
page cache and cannot establish the power-loss durability window.
"""
import random
import time

from hermit_test_lib import Client, Server, binary_from_argv, encode, expect

BINARY = binary_from_argv()
RNG = random.Random(0xDEAD)


def run_workload(client, rng, n_ops):
    """Returns the list of (key, value_or_None) states ACKed, in order."""
    history = []
    for i in range(n_ops):
        key = f"k{rng.randrange(50)}"
        if rng.random() < 0.2:
            reply = client.cmd("DEL", key)
            assert reply in (0, 1), reply
            history.append((key, None))
        else:
            val = f"v{i}:{rng.randrange(1_000_000)}"
            r = client.cmd("SET", key, val)
            assert r == "OK"
            history.append((key, val))
    return history


def shadow_at(history, prefix_len):
    state = {}
    for key, val in history[:prefix_len]:
        if val is None:
            state.pop(key, None)
        else:
            state[key] = val
    return state


def state_matches_some_prefix(server_state, history):
    for plen in range(len(history), -1, -1):
        if shadow_at(history, plen) == server_state:
            return plen
    return None


def dump_state(client):
    keys = client.cmd("KEYS", "*") or []
    return {k.decode(): client.cmd("GET", k).decode() for k in keys}


for round_no in range(5):
    n_ops = RNG.randrange(100, 400)
    with Server(BINARY, extra_args=["--wal", "--fsync=always"]) as srv:
        c = Client(srv.port)
        history = run_workload(c, RNG, n_ops)
        srv.kill9()
        c.close()

        # Same data dir, fresh process: recovery must replay snapshot + WAL.
        with Server(BINARY, extra_args=["--wal", "--fsync=always"],
                    data_dir=srv.data_dir) as srv2:
            c2 = Client(srv2.port)
            recovered = dump_state(c2)
            expect(recovered == shadow_at(history, len(history)),
                   f"round {round_no}: fsync=always lost or changed ACKed state")
            c2.close()

# One command is deliberately left without a client-observed acknowledgment.
# A unique key makes its presence distinguishable from every ACKed operation.
for round_no in range(8):
    with Server(BINARY, extra_args=["--wal", "--fsync=always"]) as srv:
        c = Client(srv.port)
        history = run_workload(c, RNG, 100)
        pending = (f"pending:{round_no}", "x" * 65536)
        c.send_raw(encode("SET", *pending))
        time.sleep(RNG.uniform(0, 0.003))
        srv.kill9()
        c.close()
        with Server(BINARY, extra_args=["--wal", "--fsync=always"],
                    data_dir=srv.data_dir) as restarted:
            c2 = Client(restarted.port)
            recovered = dump_state(c2)
            acked_state = shadow_at(history, len(history))
            with_pending = dict(acked_state)
            with_pending[pending[0]] = pending[1]
            expect(recovered == acked_state or recovered == with_pending,
                   f"in-flight round {round_no}: ACKed state changed or "
                   "unACKed command recovered only partially")
            c2.close()

# everysec: weaker guarantee — any ACKed prefix is acceptable, corruption is not.
with Server(BINARY, extra_args=["--wal", "--fsync=everysec"]) as srv:
    c = Client(srv.port)
    history = run_workload(c, RNG, 300)
    srv.kill9()
    c.close()
    with Server(BINARY, extra_args=["--wal", "--fsync=everysec"],
                data_dir=srv.data_dir) as srv2:
        c2 = Client(srv2.port)
        plen = state_matches_some_prefix(dump_state(c2), history)
        expect(plen is not None, "everysec: recovered state matches no ACKed prefix")
        c2.close()
print("OK")

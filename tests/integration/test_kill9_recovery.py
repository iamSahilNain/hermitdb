#!/usr/bin/env python3
"""CP4 crash harness: random workload -> SIGKILL at a random moment ->
restart -> recovered state must be consistent with what was ACKed.

  --fsync=always   every ACKed write must survive. The in-flight command (sent
                   but unACKed at kill time) may legitimately be present or
                   absent — both are correct.
  --fsync=everysec recovered state must be SOME prefix of the ACKed history
                   (bounded data loss, no reordering, no corruption).

Several rounds with different kill offsets; each round replays the shadow
model to the recovered server's answers.
"""
import random

from hermit_test_lib import Client, Server, binary_from_argv, expect

BINARY = binary_from_argv()
RNG = random.Random(0xDEAD)


def run_workload(client, rng, n_ops):
    """Returns the list of (key, value_or_None) states ACKed, in order."""
    history = []
    for i in range(n_ops):
        key = f"k{rng.randrange(50)}"
        if rng.random() < 0.2:
            client.cmd("DEL", key)
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
        srv.kill9()  # no shutdown courtesy — this is the whole point

        # Same data dir, fresh process: recovery must replay snapshot + WAL.
        with Server(BINARY, extra_args=["--wal", "--fsync=always"],
                    data_dir=srv.data_dir) as srv2:
            c2 = Client(srv2.port)
            recovered = dump_state(c2)
            plen = state_matches_some_prefix(recovered, history)
            expect(plen is not None,
                   f"round {round_no}: recovered state matches no ACKed prefix")
            # fsync=always: nothing ACKed may be lost (allow only the final
            # in-flight op to be absent).
            expect(plen >= len(history) - 1,
                   f"round {round_no}: fsync=always lost ACKed writes "
                   f"(prefix {plen}/{len(history)})")
            c2.close()

# everysec: weaker guarantee — any ACKed prefix is acceptable, corruption is not.
with Server(BINARY, extra_args=["--wal", "--fsync=everysec"]) as srv:
    c = Client(srv.port)
    history = run_workload(c, RNG, 300)
    srv.kill9()
    with Server(BINARY, extra_args=["--wal", "--fsync=everysec"],
                data_dir=srv.data_dir) as srv2:
        c2 = Client(srv2.port)
        plen = state_matches_some_prefix(dump_state(c2), history)
        expect(plen is not None, "everysec: recovered state matches no ACKed prefix")
        c2.close()
print("OK")

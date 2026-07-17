#!/usr/bin/env python3
"""CP3 end-to-end: SET k v EX 1 actually disappears — both lazily (GET after
deadline) and actively (DBSIZE shrinks without any access touching the keys)."""
import time

from hermit_test_lib import Client, Server, binary_from_argv, expect

with Server(binary_from_argv()) as srv:
    c = Client(srv.port)

    # Lazy path.
    expect(c.cmd("SET", "k", "v", "EX", "1") == "OK", "SET EX 1")
    expect(c.cmd("GET", "k") == b"v", "alive before deadline")
    time.sleep(1.3)
    expect(c.cmd("GET", "k") is None, "expired on access")
    expect(c.cmd("EXISTS", "k") == 0, "EXISTS agrees")

    # PEXPIRE on an existing key.
    c.cmd("SET", "p", "v")
    expect(c.cmd("PEXPIRE", "p", "300") == 1, "PEXPIRE -> :1")
    time.sleep(0.5)
    expect(c.cmd("GET", "p") is None, "PEXPIRE honored")

    # Active path: keys we never touch again must still get reclaimed.
    for i in range(500):
        c.cmd("SET", f"batch:{i}", "v", "PX", "200")
    time.sleep(3)  # several active-cycle ticks; NO access to batch:* keys
    size = c.cmd("DBSIZE")
    expect(isinstance(size, int) and size <= 25,
           f"active expiry reclaimed untouched keys (DBSIZE={size})")
    c.close()
print("OK")

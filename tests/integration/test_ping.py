#!/usr/bin/env python3
"""CP2 gate (M2): a live server must answer PING/ECHO over a real socket."""
from hermit_test_lib import Client, Server, binary_from_argv, expect

with Server(binary_from_argv()) as srv:
    c = Client(srv.port)
    expect(c.cmd("PING") == "PONG", "PING -> +PONG")
    expect(c.cmd("PING", "hello") == b"hello", "PING msg echoes as bulk")
    expect(c.cmd("ECHO", "abc") == b"abc", "ECHO round-trips")
    # Inline framing — redis-cli sends these.
    c.send_raw(b"PING\r\n")
    expect(c.read_reply() == "PONG", "inline PING works")
    c.close()
print("OK")

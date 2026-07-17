#!/usr/bin/env python3
"""CP2: a client that never drains its replies must not block other clients.
This is the short-write/EPOLLOUT part of the event-loop contract: when the
kernel send buffer to the slow client fills, the server must park the pending
bytes and keep serving everyone else."""
import socket
import time

from hermit_test_lib import Client, Server, binary_from_argv, encode, expect

with Server(binary_from_argv()) as srv:
    # Slow reader: tiny receive buffer, sends a flood of PINGs, never recv()s.
    slow = socket.create_connection(("127.0.0.1", srv.port))
    slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    payload = b"x" * 1024
    flood = b"".join(encode("PING", payload) for _ in range(5000))
    try:
        slow.sendall(flood)
    except (BrokenPipeError, ConnectionResetError):
        pass  # acceptable: server may cap and drop an abusive client

    # Meanwhile a well-behaved client must get sub-second service.
    fast = Client(srv.port, timeout=5)
    start = time.time()
    for i in range(100):
        expect(fast.cmd("PING") == "PONG", f"fast client blocked at iteration {i}")
    elapsed = time.time() - start
    expect(elapsed < 5.0, f"fast client took {elapsed:.1f}s behind a slow reader")
    fast.close()
    slow.close()

    # Abrupt disconnect mid-command must not wedge the server either.
    rude = socket.create_connection(("127.0.0.1", srv.port))
    rude.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk")  # partial command...
    rude.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                    b"\x01\x00\x00\x00\x00\x00\x00\x00")  # ...then RST
    rude.close()
    check = Client(srv.port)
    expect(check.cmd("PING") == "PONG", "server survived an RST mid-command")
    check.close()
print("OK")

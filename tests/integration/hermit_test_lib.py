"""Shared harness for hermitdb integration tests. [Claude Code]

Spawns the real server binary (argv[1] of each test) and talks RESP2 over
raw sockets — deliberately NOT redis-py, so the tests exercise our framing
end-to-end with zero dependencies.
"""
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    """Context manager around a hermitdb process."""

    def __init__(self, binary, extra_args=None, data_dir=None):
        self.binary = binary
        self.port = free_port()
        self.data_dir = data_dir or tempfile.mkdtemp(prefix="hermit_it_")
        self._owns_data_dir = data_dir is None
        self.args = [binary, f"--port={self.port}", f"--data-dir={self.data_dir}"]
        self.args += extra_args or []
        self.proc = None

    def __enter__(self):
        self.start()
        return self

    def start(self):
        os.makedirs(self.data_dir, exist_ok=True)
        self.proc = subprocess.Popen(
            self.args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out, err = self.proc.communicate()
                raise RuntimeError(
                    f"server exited rc={self.proc.returncode} before accepting "
                    f"connections\nstdout: {out.decode()}\nstderr: {err.decode()}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start listening within 10s")

    def kill9(self):
        self.proc.kill()  # SIGKILL: the crash-recovery scenario
        self.proc.wait()

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self._owns_data_dir:
            shutil.rmtree(self.data_dir, ignore_errors=True)
        return False


def encode(*args):
    """RESP2 array-of-bulk-strings encoding of one command."""
    out = b"*%d\r\n" % len(args)
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Client:
    def __init__(self, port, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def send_raw(self, data):
        self.sock.sendall(data)

    def cmd(self, *args):
        self.send_raw(encode(*args))
        return self.read_reply()

    def _read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed connection")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def read_reply(self):
        """Returns: str for +simple, RespError for -err, int for :n,
        bytes/None for bulk, list for arrays."""
        line = self._read_line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return RespError(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n == -1:
                return None
            data = self._read_exact(n)
            assert self._read_exact(2) == b"\r\n"
            return data
        if kind == b"*":
            n = int(rest)
            if n == -1:
                return None
            return [self.read_reply() for _ in range(n)]
        raise ValueError(f"unparseable reply line: {line!r}")

    def close(self):
        self.sock.close()


class RespError:
    def __init__(self, message):
        self.message = message

    def __repr__(self):
        return f"RespError({self.message!r})"


def expect(cond, msg):
    if not cond:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)


def binary_from_argv():
    if len(sys.argv) < 2:
        print("usage: <test>.py /path/to/hermitdb", file=sys.stderr)
        sys.exit(2)
    return sys.argv[1]


def rand_key(rng, n=1000):
    return f"key:{rng.randrange(n)}"

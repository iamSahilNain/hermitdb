// M3: Tier 1 command semantics via direct dispatcher calls — no sockets, no
// parser (commands are constructed as argv vectors; replies checked as exact
// RESP wire literals). TTL-state assertions that depend on CP3 live in the
// cp3-labeled tests; everything here holds with the CP3 stub in place.

#include "core/commands.h"

#include <catch2/catch_test_macros.hpp>

#include "core/db.h"
#include "core/expiry.h"
#include "util/clock.h"

using namespace hermit;
using core::CommandDispatcher;
using core::Db;
using core::ExpiryManager;

namespace {

struct Fixture {
  Db db;
  ManualClock clock;
  ExpiryManager expiry{db, clock};
  CommandDispatcher d{db, expiry, clock};

  std::string run(std::vector<std::string> argv) { return d.execute(argv); }
};

constexpr const char* kWrongType =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
constexpr const char* kNotInt = "-ERR value is not an integer or out of range\r\n";

}  // namespace

TEST_CASE("m3 cmd: PING/ECHO") {
  Fixture f;
  CHECK(f.run({"PING"}) == "+PONG\r\n");
  CHECK(f.run({"ping"}) == "+PONG\r\n");  // case-insensitive
  CHECK(f.run({"PING", "hi"}) == "$2\r\nhi\r\n");
  CHECK(f.run({"ECHO", "abc"}) == "$3\r\nabc\r\n");
  CHECK(f.run({"ECHO"}) == "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST_CASE("m3 cmd: SET/GET basics") {
  Fixture f;
  CHECK(f.run({"SET", "k", "v"}) == "+OK\r\n");
  CHECK(f.run({"GET", "k"}) == "$1\r\nv\r\n");
  CHECK(f.run({"GET", "missing"}) == "$-1\r\n");
  CHECK(f.run({"SET", "k", "v2"}) == "+OK\r\n");
  CHECK(f.run({"GET", "k"}) == "$2\r\nv2\r\n");
  CHECK(f.run({"GET"}) == "-ERR wrong number of arguments for 'get' command\r\n");
  CHECK(f.run({"SET", "k"}) == "-ERR wrong number of arguments for 'set' command\r\n");
  // Binary-safe value round-trip.
  CHECK(f.run({"SET", "b", std::string("x\r\ny", 4)}) == "+OK\r\n");
  CHECK(f.run({"GET", "b"}) == "$4\r\nx\r\ny\r\n");
}

TEST_CASE("m3 cmd: SET options NX/XX/EX/PX") {
  Fixture f;
  CHECK(f.run({"SET", "k", "1", "NX"}) == "+OK\r\n");
  CHECK(f.run({"SET", "k", "2", "NX"}) == "$-1\r\n");
  CHECK(f.run({"GET", "k"}) == "$1\r\n1\r\n");
  CHECK(f.run({"SET", "k", "3", "XX"}) == "+OK\r\n");
  CHECK(f.run({"SET", "absent", "1", "XX"}) == "$-1\r\n");
  CHECK(f.run({"SET", "t", "v", "EX", "100"}) == "+OK\r\n");
  CHECK(f.run({"SET", "t", "v", "PX", "100000"}) == "+OK\r\n");
  CHECK(f.run({"SET", "t", "v", "ex", "100"}) == "+OK\r\n");  // options case-insensitive
  CHECK(f.run({"SET", "t", "v", "EX", "0"}) == "-ERR invalid expire time in 'set' command\r\n");
  CHECK(f.run({"SET", "t", "v", "EX", "-1"}) == "-ERR invalid expire time in 'set' command\r\n");
  CHECK(f.run({"SET", "t", "v", "EX", "abc"}) == kNotInt);
  CHECK(f.run({"SET", "t", "v", "BOGUS"}) == "-ERR syntax error\r\n");
  CHECK(f.run({"SET", "t", "v", "NX", "XX"}) == "-ERR syntax error\r\n");
  CHECK(f.run({"SET", "t", "v", "EX"}) == "-ERR syntax error\r\n");
}

TEST_CASE("m3 cmd: DEL/EXISTS are variadic") {
  Fixture f;
  f.run({"SET", "a", "1"});
  f.run({"SET", "b", "2"});
  CHECK(f.run({"EXISTS", "a", "b", "missing", "a"}) == ":3\r\n");
  CHECK(f.run({"DEL", "a", "missing", "b"}) == ":2\r\n");
  CHECK(f.run({"EXISTS", "a", "b"}) == ":0\r\n");
  CHECK(f.run({"DEL"}) == "-ERR wrong number of arguments for 'del' command\r\n");
  CHECK(f.run({"EXISTS"}) == "-ERR wrong number of arguments for 'exists' command\r\n");
}

TEST_CASE("m3 cmd: INCR/DECR/INCRBY") {
  Fixture f;
  CHECK(f.run({"INCR", "n"}) == ":1\r\n");
  CHECK(f.run({"INCR", "n"}) == ":2\r\n");
  CHECK(f.run({"INCRBY", "n", "40"}) == ":42\r\n");
  CHECK(f.run({"DECR", "n"}) == ":41\r\n");
  CHECK(f.run({"INCRBY", "n", "-41"}) == ":0\r\n");
  CHECK(f.run({"GET", "n"}) == "$1\r\n0\r\n");

  f.run({"SET", "s", "notanumber"});
  CHECK(f.run({"INCR", "s"}) == kNotInt);
  f.run({"SET", "s", "12 "});  // trailing junk is not an integer
  CHECK(f.run({"INCR", "s"}) == kNotInt);
  CHECK(f.run({"INCRBY", "n", "notanumber"}) == kNotInt);

  // Overflow at both rails.
  f.run({"SET", "max", "9223372036854775807"});
  CHECK(f.run({"INCR", "max"}) == kNotInt);
  f.run({"SET", "min", "-9223372036854775808"});
  CHECK(f.run({"DECR", "min"}) == kNotInt);
  CHECK(f.run({"GET", "max"}) == "$19\r\n9223372036854775807\r\n");  // unchanged on error
}

TEST_CASE("m3 cmd: TYPE") {
  Fixture f;
  f.run({"SET", "s", "v"});
  f.run({"RPUSH", "l", "a"});
  CHECK(f.run({"TYPE", "s"}) == "+string\r\n");
  CHECK(f.run({"TYPE", "l"}) == "+list\r\n");
  CHECK(f.run({"TYPE", "missing"}) == "+none\r\n");
}

TEST_CASE("m3 cmd: WRONGTYPE guards") {
  Fixture f;
  f.run({"RPUSH", "l", "a"});
  CHECK(f.run({"GET", "l"}) == kWrongType);
  CHECK(f.run({"INCR", "l"}) == kWrongType);
  f.run({"SET", "s", "v"});
  CHECK(f.run({"LPUSH", "s", "x"}) == kWrongType);
  CHECK(f.run({"LLEN", "s"}) == kWrongType);
  CHECK(f.run({"LRANGE", "s", "0", "-1"}) == kWrongType);
  CHECK(f.run({"LPOP", "s"}) == kWrongType);
}

TEST_CASE("m3 cmd: KEYS/DBSIZE/FLUSHALL") {
  Fixture f;
  CHECK(f.run({"DBSIZE"}) == ":0\r\n");
  f.run({"SET", "glob:1", "a"});
  f.run({"SET", "glob:2", "b"});
  f.run({"SET", "other", "c"});
  CHECK(f.run({"DBSIZE"}) == ":3\r\n");

  std::string reply = f.run({"KEYS", "glob:*"});
  // Order is unspecified (hash map) — accept either.
  const std::string a = "*2\r\n$6\r\nglob:1\r\n$6\r\nglob:2\r\n";
  const std::string b = "*2\r\n$6\r\nglob:2\r\n$6\r\nglob:1\r\n";
  CHECK((reply == a || reply == b));
  CHECK(f.run({"KEYS", "nomatch*"}) == "*0\r\n");

  CHECK(f.run({"FLUSHALL"}) == "+OK\r\n");
  CHECK(f.run({"DBSIZE"}) == ":0\r\n");
}

TEST_CASE("m3 cmd: TTL family (CP3-independent surface)") {
  Fixture f;
  // Missing key: -2 everywhere it applies.
  CHECK(f.run({"TTL", "missing"}) == ":-2\r\n");
  CHECK(f.run({"PTTL", "missing"}) == ":-2\r\n");
  CHECK(f.run({"EXPIRE", "missing", "10"}) == ":0\r\n");
  CHECK(f.run({"PEXPIRE", "missing", "10"}) == ":0\r\n");
  CHECK(f.run({"PERSIST", "missing"}) == ":0\r\n");

  f.run({"SET", "k", "v"});
  // No TTL armed (and with the CP3 stub, none can be): -1 / persist 0.
  CHECK(f.run({"TTL", "k"}) == ":-1\r\n");
  CHECK(f.run({"PTTL", "k"}) == ":-1\r\n");
  CHECK(f.run({"PERSIST", "k"}) == ":0\r\n");
  // Arming an expiry on an existing key acks :1 (deadline bookkeeping is CP3).
  CHECK(f.run({"EXPIRE", "k", "100"}) == ":1\r\n");
  CHECK(f.run({"PEXPIRE", "k", "100000"}) == ":1\r\n");
  // Non-positive expiry deletes immediately (pure dispatcher+db behavior).
  CHECK(f.run({"EXPIRE", "k", "-1"}) == ":1\r\n");
  CHECK(f.run({"GET", "k"}) == "$-1\r\n");
  CHECK(f.run({"EXPIRE", "k", "abc"}) == kNotInt);
  CHECK(f.run({"TTL"}) == "-ERR wrong number of arguments for 'ttl' command\r\n");
}

TEST_CASE("m3 cmd: CONFIG GET / COMMAND stubs") {
  Fixture f;
  CHECK(f.run({"CONFIG", "GET", "save"}) == "*2\r\n$4\r\nsave\r\n$0\r\n\r\n");
  CHECK(f.run({"CONFIG", "GET", "appendonly"}) == "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n");
  CHECK(f.run({"CONFIG", "GET", "unknown-param"}) == "*0\r\n");
  CHECK(f.run({"CONFIG", "SET", "x", "y"}) == "-ERR CONFIG subcommand must be GET\r\n");
  CHECK(f.run({"COMMAND"}) == "*0\r\n");
  CHECK(f.run({"COMMAND", "DOCS"}) == "*0\r\n");
}

TEST_CASE("m3 cmd: SHUTDOWN sets the flag; unknown commands error") {
  Fixture f;
  CHECK_FALSE(f.d.shutdown_requested());
  CHECK(f.run({"NOSUCHCMD", "x"}) == "-ERR unknown command 'NOSUCHCMD'\r\n");
  CHECK(f.run({"SHUTDOWN"}) == "+OK\r\n");
  CHECK(f.d.shutdown_requested());
}

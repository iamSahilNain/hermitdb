// M3/Tier 2: list command semantics via direct dispatcher calls.

#include "core/commands.h"

#include <catch2/catch_test_macros.hpp>

#include "core/db.h"
#include "core/expiry.h"
#include "util/clock.h"

using namespace hermit;

namespace {

struct Fixture {
  core::Db db;
  ManualClock clock;
  core::ExpiryManager expiry{db, clock};
  core::CommandDispatcher d{db, expiry, clock};

  std::string run(std::vector<std::string> argv) { return d.execute(argv); }
};

}  // namespace

TEST_CASE("m3 lists: push returns new length; LPUSH prepends left-to-right") {
  Fixture f;
  CHECK(f.run({"RPUSH", "l", "a"}) == ":1\r\n");
  CHECK(f.run({"RPUSH", "l", "b", "c"}) == ":3\r\n");        // variadic
  CHECK(f.run({"LPUSH", "l", "x", "y"}) == ":5\r\n");        // y, x, a, b, c
  CHECK(f.run({"LRANGE", "l", "0", "-1"}) ==
        "*5\r\n$1\r\ny\r\n$1\r\nx\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
  CHECK(f.run({"LLEN", "l"}) == ":5\r\n");
  CHECK(f.run({"LPUSH", "l"}) == "-ERR wrong number of arguments for 'lpush' command\r\n");
}

TEST_CASE("m3 lists: pops from both ends; empty list deletes the key") {
  Fixture f;
  f.run({"RPUSH", "l", "a", "b"});
  CHECK(f.run({"LPOP", "l"}) == "$1\r\na\r\n");
  CHECK(f.run({"RPOP", "l"}) == "$1\r\nb\r\n");
  CHECK(f.run({"EXISTS", "l"}) == ":0\r\n");
  CHECK(f.run({"TYPE", "l"}) == "+none\r\n");
  CHECK(f.run({"LPOP", "l"}) == "$-1\r\n");
  CHECK(f.run({"RPOP", "missing"}) == "$-1\r\n");
  CHECK(f.run({"LLEN", "missing"}) == ":0\r\n");
}

TEST_CASE("m3 lists: LRANGE index handling at the command surface") {
  Fixture f;
  f.run({"RPUSH", "l", "a", "b", "c", "d", "e"});
  CHECK(f.run({"LRANGE", "l", "1", "3"}) == "*3\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n");
  CHECK(f.run({"LRANGE", "l", "-2", "-1"}) == "*2\r\n$1\r\nd\r\n$1\r\ne\r\n");
  CHECK(f.run({"LRANGE", "l", "3", "1"}) == "*0\r\n");
  CHECK(f.run({"LRANGE", "missing", "0", "-1"}) == "*0\r\n");
  CHECK(f.run({"LRANGE", "l", "abc", "1"}) ==
        "-ERR value is not an integer or out of range\r\n");
  CHECK(f.run({"LRANGE", "l", "0"}) ==
        "-ERR wrong number of arguments for 'lrange' command\r\n");
}

TEST_CASE("m3 lists: binary-safe elements") {
  Fixture f;
  const std::string blob("a\r\n\0b", 5);
  f.run({"RPUSH", "l", blob});
  CHECK(f.run({"LPOP", "l"}) == "$5\r\n" + blob + "\r\n");
}

TEST_CASE("m3 lists: DEL and FLUSHALL apply to lists too") {
  Fixture f;
  f.run({"RPUSH", "l", "a"});
  CHECK(f.run({"DEL", "l"}) == ":1\r\n");
  f.run({"RPUSH", "l2", "a"});
  CHECK(f.run({"FLUSHALL"}) == "+OK\r\n");
  CHECK(f.run({"DBSIZE"}) == ":0\r\n");
}

#include "protocol/resp_writer.h"

#include <catch2/catch_test_macros.hpp>

using namespace hermit;

TEST_CASE("simple strings") {
  CHECK(resp::simple("OK") == "+OK\r\n");
  CHECK(resp::simple("PONG") == "+PONG\r\n");
}

TEST_CASE("errors") {
  CHECK(resp::error("ERR wrong number of arguments for 'get' command") ==
        "-ERR wrong number of arguments for 'get' command\r\n");
  CHECK(resp::error("WRONGTYPE Operation against a key holding the wrong kind of value") ==
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
}

TEST_CASE("integers") {
  CHECK(resp::integer(0) == ":0\r\n");
  CHECK(resp::integer(42) == ":42\r\n");
  CHECK(resp::integer(-7) == ":-7\r\n");
  CHECK(resp::integer(INT64_MAX) == ":9223372036854775807\r\n");
}

TEST_CASE("bulk strings") {
  CHECK(resp::bulk("hello") == "$5\r\nhello\r\n");
  CHECK(resp::bulk("") == "$0\r\n\r\n");
  // Binary safety: payload containing CRLF must round-trip by length.
  CHECK(resp::bulk("a\r\nb") == "$4\r\na\r\nb\r\n");
  std::string with_nul("x\0y", 3);
  CHECK(resp::bulk(with_nul) == std::string("$3\r\nx\0y\r\n", 9));
}

TEST_CASE("nils") {
  CHECK(resp::null_bulk() == "$-1\r\n");
  CHECK(resp::null_array() == "*-1\r\n");
  CHECK(resp::empty_array() == "*0\r\n");
}

TEST_CASE("arrays concatenate pre-encoded elements") {
  CHECK(resp::array({resp::bulk("a"), resp::bulk("b")}) == "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
  CHECK(resp::array({resp::integer(1), resp::null_bulk()}) == "*2\r\n:1\r\n$-1\r\n");
  CHECK(resp::array({}) == "*0\r\n");
}

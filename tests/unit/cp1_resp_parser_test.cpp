// CHECKPOINT 1 acceptance suite. Written by Claude Code; FAILS until the
// human implements protocol/resp_parser.cpp. Run:
//   ctest --test-dir build -L cp1 --output-on-failure

#include "protocol/resp_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <vector>

using hermit::protocol::Command;
using hermit::protocol::ParseResult;
using hermit::protocol::ParseStatus;
using hermit::protocol::RespParser;

namespace {

constexpr std::size_t kMaxFrame = 1 << 20;  // 1 MiB cap for these tests

// Feeds `input` in the given chunk sizes; returns all commands, requiring
// every intermediate status to be kOk.
std::vector<Command> feed_chunked(const std::string& input, std::size_t chunk) {
  RespParser p(kMaxFrame);
  std::vector<Command> got;
  for (std::size_t i = 0; i < input.size(); i += chunk) {
    ParseResult r = p.feed(std::string_view(input).substr(i, chunk));
    REQUIRE(r.status == ParseStatus::kOk);
    for (auto& c : r.commands) got.push_back(std::move(c));
  }
  return got;
}

// (encoded bytes, expected commands) — the shared corpus.
const std::vector<std::pair<std::string, std::vector<Command>>>& corpus() {
  static const std::vector<std::pair<std::string, std::vector<Command>>> kCorpus = {
      {"*1\r\n$4\r\nPING\r\n", {{"PING"}}},
      {"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n", {{"SET", "key", "hello"}}},
      {"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n", {{"GET", "key"}}},
      // Empty bulk payload is a legal argument.
      {"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n", {{"SET", "k", ""}}},
      // Binary-safe: the bulk payload contains CRLF and must be length-framed.
      {"*2\r\n$4\r\nECHO\r\n$12\r\nhello\r\nworld\r\n", {{"ECHO", "hello\r\nworld"}}},
      // Inline commands (redis-cli uses these).
      {"PING\r\n", {{"PING"}}},
      {"SET k v\r\n", {{"SET", "k", "v"}}},
  };
  return kCorpus;
}

}  // namespace

TEST_CASE("cp1: whole-buffer parse of the corpus") {
  for (const auto& [encoded, expected] : corpus()) {
    INFO("input: " << encoded);
    RespParser p(kMaxFrame);
    ParseResult r = p.feed(encoded);
    REQUIRE(r.status == ParseStatus::kOk);
    CHECK(r.commands == expected);
  }
}

TEST_CASE("cp1: byte-at-a-time — TCP gives no message boundaries") {
  for (const auto& [encoded, expected] : corpus()) {
    INFO("input: " << encoded);
    CHECK(feed_chunked(encoded, 1) == expected);
  }
}

TEST_CASE("cp1: every split point of a representative command") {
  const std::string input = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n";
  const std::vector<Command> expected = {{"SET", "key", "hello"}};
  for (std::size_t split = 1; split < input.size(); ++split) {
    INFO("split at byte " << split);
    RespParser p(kMaxFrame);
    ParseResult a = p.feed(std::string_view(input).substr(0, split));
    REQUIRE(a.status == ParseStatus::kOk);
    ParseResult b = p.feed(std::string_view(input).substr(split));
    REQUIRE(b.status == ParseStatus::kOk);
    std::vector<Command> got = a.commands;
    got.insert(got.end(), b.commands.begin(), b.commands.end());
    CHECK(got == expected);
  }
}

TEST_CASE("cp1: pipelining — many commands in one read") {
  std::string blob;
  std::vector<Command> expected;
  for (const auto& [encoded, cmds] : corpus()) {
    blob += encoded;
    expected.insert(expected.end(), cmds.begin(), cmds.end());
  }
  RespParser p(kMaxFrame);
  ParseResult r = p.feed(blob);
  REQUIRE(r.status == ParseStatus::kOk);
  CHECK(r.commands == expected);
}

TEST_CASE("cp1: randomized chunking fuzz (seeded, reproducible)") {
  std::string blob;
  std::vector<Command> expected;
  for (const auto& [encoded, cmds] : corpus()) {
    blob += encoded;
    expected.insert(expected.end(), cmds.begin(), cmds.end());
  }
  std::mt19937 rng(0xC0FFEE);
  for (int round = 0; round < 50; ++round) {
    RespParser p(kMaxFrame);
    std::vector<Command> got;
    std::size_t i = 0;
    while (i < blob.size()) {
      std::uniform_int_distribution<std::size_t> d(1, 7);
      std::size_t n = std::min(d(rng), blob.size() - i);
      ParseResult r = p.feed(std::string_view(blob).substr(i, n));
      REQUIRE(r.status == ParseStatus::kOk);
      for (auto& c : r.commands) got.push_back(std::move(c));
      i += n;
    }
    REQUIRE(got == expected);
  }
}

TEST_CASE("cp1: interleaved array and inline framings on one connection") {
  RespParser p(kMaxFrame);
  ParseResult r = p.feed("PING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\nSET a b\r\n");
  REQUIRE(r.status == ParseStatus::kOk);
  CHECK(r.commands == std::vector<Command>{{"PING"}, {"ECHO", "hi"}, {"SET", "a", "b"}});
}

TEST_CASE("cp1: empty frames produce no commands and no errors") {
  RespParser p(kMaxFrame);
  SECTION("*0") {
    ParseResult r = p.feed("*0\r\n");
    REQUIRE(r.status == ParseStatus::kOk);
    CHECK(r.commands.empty());
  }
  SECTION("blank inline line") {
    ParseResult r = p.feed("\r\n");
    REQUIRE(r.status == ParseStatus::kOk);
    CHECK(r.commands.empty());
  }
  SECTION("zero bytes") {
    ParseResult r = p.feed("");
    REQUIRE(r.status == ParseStatus::kOk);
    CHECK(r.commands.empty());
  }
}

TEST_CASE("cp1: partial input yields nothing until completed") {
  RespParser p(kMaxFrame);
  ParseResult r1 = p.feed("*2\r\n$3\r\nGET\r\n$3\r\nke");
  REQUIRE(r1.status == ParseStatus::kOk);
  CHECK(r1.commands.empty());
  ParseResult r2 = p.feed("y\r\n");
  REQUIRE(r2.status == ParseStatus::kOk);
  CHECK(r2.commands == std::vector<Command>{{"GET", "key"}});
}

TEST_CASE("cp1: protocol errors are reported, not crashed on") {
  SECTION("non-numeric array length") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*abc\r\n").status == ParseStatus::kProtocolError);
  }
  SECTION("non-numeric bulk length") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*1\r\n$xy\r\n").status == ParseStatus::kProtocolError);
  }
  SECTION("negative bulk length inside a command") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*2\r\n$3\r\nGET\r\n$-1\r\n").status == ParseStatus::kProtocolError);
  }
  SECTION("bulk payload not terminated by CRLF") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*1\r\n$4\r\nPINGXX").status == ParseStatus::kProtocolError);
  }
  SECTION("expected '$' header, got something else") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*1\r\n:123\r\n").status == ParseStatus::kProtocolError);
  }
}

TEST_CASE("cp1: memory-blowup defense — huge declared sizes die before allocation") {
  SECTION("giant array header") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*1000000000\r\n").status == ParseStatus::kProtocolError);
  }
  SECTION("giant bulk header") {
    RespParser p(kMaxFrame);
    CHECK(p.feed("*1\r\n$999999999\r\n").status == ParseStatus::kProtocolError);
  }
  SECTION("unterminated inline line growing past the cap") {
    RespParser p(1024);  // tiny cap to keep the test fast
    std::string junk(4096, 'A');
    ParseResult r = p.feed(junk);
    CHECK(r.status == ParseStatus::kProtocolError);
  }
}

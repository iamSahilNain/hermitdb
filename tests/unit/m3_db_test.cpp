// M3: Db keyspace semantics — pure data-structure tests, no sockets, no
// parser, no checkpoint code. Label: m3 (required in CI).

#include "core/db.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using hermit::core::Db;

TEST_CASE("m3 db: string set/get/del/exists") {
  Db db;
  CHECK_FALSE(db.get("k").has_value());
  CHECK_FALSE(db.exists("k"));
  CHECK(db.type("k") == Db::Type::kNone);

  db.set("k", "v");
  REQUIRE(db.get("k").has_value());
  CHECK(*db.get("k") == "v");
  CHECK(db.exists("k"));
  CHECK(db.type("k") == Db::Type::kString);
  CHECK(db.size() == 1);

  db.set("k", "v2");  // overwrite
  CHECK(*db.get("k") == "v2");
  CHECK(db.size() == 1);

  CHECK(db.del("k"));
  CHECK_FALSE(db.del("k"));
  CHECK(db.size() == 0);
}

TEST_CASE("m3 db: binary-safe keys and values") {
  Db db;
  const std::string key("k\r\n\0key", 7);
  const std::string val("v\0\xff", 3);
  db.set(key, val);
  REQUIRE(db.get(key).has_value());
  CHECK(*db.get(key) == val);
}

TEST_CASE("m3 db: set overwrites a list with a string (Redis SET semantics)") {
  Db db;
  db.rpush("k", "a");
  CHECK(db.type("k") == Db::Type::kList);
  db.set("k", "s");
  CHECK(db.type("k") == Db::Type::kString);
  CHECK(*db.get("k") == "s");
}

TEST_CASE("m3 db: clear") {
  Db db;
  db.set("a", "1");
  db.rpush("l", "x");
  db.clear();
  CHECK(db.size() == 0);
  CHECK_FALSE(db.exists("a"));
  CHECK_FALSE(db.exists("l"));
}

TEST_CASE("m3 db: glob matching") {
  Db db;
  for (const char* k : {"glob:1", "glob:2", "other", "gx", "g", "kay", "key", "kez", "[lit"})
    db.set(k, "v");

  auto sorted = [&](const std::string& pat) {
    auto v = db.keys_matching_glob(pat);
    std::sort(v.begin(), v.end());
    return v;
  };

  CHECK(sorted("glob:*") == std::vector<std::string>{"glob:1", "glob:2"});
  CHECK(sorted("g*") == std::vector<std::string>{"g", "glob:1", "glob:2", "gx"});
  CHECK(sorted("ke?") == std::vector<std::string>{"key", "kez"});
  CHECK(sorted("k[ae]y") == std::vector<std::string>{"kay", "key"});
  CHECK(sorted("ke[x-z]") == std::vector<std::string>{"key", "kez"});
  CHECK(sorted("ke[^z]") == std::vector<std::string>{"key"});
  CHECK(sorted("*") ==
        std::vector<std::string>{"[lit", "g", "glob:1", "glob:2", "gx", "kay", "key", "kez",
                                 "other"});
  CHECK(sorted("nomatch*").empty());
  // Escaped star is a literal star, not a wildcard.
  db.set("a*b", "v");
  CHECK(sorted("a\\*b") == std::vector<std::string>{"a*b"});
  // Unclosed bracket matches a literal '['.
  CHECK(sorted("[lit") == std::vector<std::string>{"[lit"});
}

TEST_CASE("m3 db: list push/pop/len ordering") {
  Db db;
  CHECK(db.lpush("l", "b") == 1);
  CHECK(db.lpush("l", "a") == 2);   // LPUSH prepends: a, b
  CHECK(db.rpush("l", "c") == 3);   // a, b, c
  CHECK(db.llen("l") == 3);
  CHECK(db.type("l") == Db::Type::kList);

  REQUIRE(db.lpop("l").has_value());
  CHECK(*db.lpop("l") == "b");      // popped a, then b
  CHECK(*db.rpop("l") == "c");
  // Last element gone => key gone (Redis removes empty lists).
  CHECK_FALSE(db.exists("l"));
  CHECK(db.llen("l") == 0);
  CHECK_FALSE(db.lpop("l").has_value());
  CHECK_FALSE(db.rpop("l").has_value());
}

TEST_CASE("m3 db: lrange index semantics") {
  Db db;
  for (const char* v : {"a", "b", "c", "d", "e"}) db.rpush("l", v);

  CHECK(db.lrange("l", 0, -1) == std::vector<std::string>{"a", "b", "c", "d", "e"});
  CHECK(db.lrange("l", 1, 3) == std::vector<std::string>{"b", "c", "d"});
  CHECK(db.lrange("l", -2, -1) == std::vector<std::string>{"d", "e"});
  CHECK(db.lrange("l", 0, 999) == std::vector<std::string>{"a", "b", "c", "d", "e"});
  CHECK(db.lrange("l", -999, 0) == std::vector<std::string>{"a"});
  CHECK(db.lrange("l", 3, 1).empty());
  CHECK(db.lrange("l", 5, 10).empty());
  CHECK(db.lrange("missing", 0, -1).empty());
}

TEST_CASE("m3 db: restore_entry rebuilds state for snapshot load") {
  Db db;
  db.restore_entry("s", Db::Entry{std::string("v")});
  db.restore_entry("l", Db::Entry{std::deque<std::string>{"a", "b"}});
  CHECK(*db.get("s") == "v");
  CHECK(db.lrange("l", 0, -1) == std::vector<std::string>{"a", "b"});
  CHECK(db.entries().size() == 2);
}

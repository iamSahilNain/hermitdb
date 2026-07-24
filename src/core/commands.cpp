#include "core/commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "persist/wal.h"
#include "protocol/resp_writer.h"

namespace hermit::core {
namespace {

using protocol::Command;

// Everything a handler may touch. WAL is deliberately absent: WHERE the
// append happens relative to the reply is CP4's trade-off to place (SPEC §5),
// so no call site is pre-wired here.
struct Ctx {
  Db& db;
  ExpiryManager& expiry;
  const Clock& clock;
};

// FIXME(DECISION-3): relative expirations (EX/PX/EXPIRE) need "now" to build
// the absolute deadline the frozen ExpiryManager interface takes. Wall time
// is used at THIS seam only; if CP3 resolves to steady (or hybrid), this is
// the single call site to change. See INTERFACE_PROPOSALS.md for the
// set_expiry_in() proposal that would move the choice inside CP3 entirely.
int64_t now_ms(const Clock& clock) { return clock.wall_ms(); }

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

std::string wrong_arity(const std::string& name_lower) {
  return resp::error("ERR wrong number of arguments for '" + name_lower + "' command");
}

std::string wrongtype() {
  return resp::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string not_an_integer() {
  return resp::error("ERR value is not an integer or out of range");
}

// Strict Redis-style integer parse: the WHOLE string is a base-10 int64.
bool parse_i64(std::string_view s, int64_t& out) {
  if (s.empty()) return false;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, out);
  return ec == std::errc() && ptr == last;
}

// Lazy-expiry gate: every key access funnels through this before touching Db
// (the CP3 contract). With the CP3 stub it is a no-op returning false.
void touch(Ctx& c, const std::string& key) { c.expiry.check_and_expire(key); }

// ---- strings ---------------------------------------------------------------

std::string cmd_get(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("get");
  touch(c, cmd[1]);
  if (c.db.type(cmd[1]) == Db::Type::kList) return wrongtype();
  auto v = c.db.get(cmd[1]);
  return v ? resp::bulk(*v) : resp::null_bulk();
}

std::string cmd_set(Ctx& c, const Command& cmd) {
  if (cmd.size() < 3) return wrong_arity("set");
  bool nx = false, xx = false;
  bool have_expire = false;
  int64_t expire_ms = 0;
  for (std::size_t i = 3; i < cmd.size(); ++i) {
    const std::string opt = upper(cmd[i]);
    if (opt == "NX" && !xx) {
      nx = true;
    } else if (opt == "XX" && !nx) {
      xx = true;
    } else if ((opt == "EX" || opt == "PX") && !have_expire && i + 1 < cmd.size()) {
      int64_t n = 0;
      if (!parse_i64(cmd[i + 1], n)) return not_an_integer();
      if (n <= 0) return resp::error("ERR invalid expire time in 'set' command");
      expire_ms = (opt == "EX") ? n * 1000 : n;
      have_expire = true;
      ++i;
    } else {
      return resp::error("ERR syntax error");
    }
  }

  touch(c, cmd[1]);
  const bool exists = c.db.exists(cmd[1]);
  if ((nx && exists) || (xx && !exists)) return resp::null_bulk();

  c.db.set(cmd[1], cmd[2]);
  if (have_expire) {
    c.expiry.set_expiry(cmd[1], now_ms(c.clock) + expire_ms);
  } else {
    c.expiry.persist(cmd[1]);  // plain SET discards any TTL (Redis semantics)
  }
  return resp::simple("OK");
}

std::string incr_by(Ctx& c, const std::string& name, const std::string& key, int64_t delta) {
  touch(c, key);
  (void)name;
  if (c.db.type(key) == Db::Type::kList) return wrongtype();
  int64_t cur = 0;
  if (auto v = c.db.get(key)) {
    if (!parse_i64(*v, cur)) return not_an_integer();
  }
  int64_t next = 0;
  if (__builtin_add_overflow(cur, delta, &next)) return not_an_integer();
  c.db.set(key, std::to_string(next));  // db.set alone: INCR keeps the TTL
  return resp::integer(next);
}

std::string cmd_incr(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("incr");
  return incr_by(c, "incr", cmd[1], 1);
}

std::string cmd_decr(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("decr");
  return incr_by(c, "decr", cmd[1], -1);
}

std::string cmd_incrby(Ctx& c, const Command& cmd) {
  if (cmd.size() != 3) return wrong_arity("incrby");
  int64_t delta = 0;
  if (!parse_i64(cmd[2], delta)) return not_an_integer();
  return incr_by(c, "incrby", cmd[1], delta);
}

// ---- generic keyspace ------------------------------------------------------

std::string cmd_del(Ctx& c, const Command& cmd) {
  if (cmd.size() < 2) return wrong_arity("del");
  int64_t n = 0;
  for (std::size_t i = 1; i < cmd.size(); ++i) {
    touch(c, cmd[i]);
    if (c.db.del(cmd[i])) {
      c.expiry.persist(cmd[i]);  // a deleted key must not leave a TTL behind
      ++n;
    }
  }
  return resp::integer(n);
}

std::string cmd_exists(Ctx& c, const Command& cmd) {
  if (cmd.size() < 2) return wrong_arity("exists");
  int64_t n = 0;
  for (std::size_t i = 1; i < cmd.size(); ++i) {
    touch(c, cmd[i]);
    if (c.db.exists(cmd[i])) ++n;
  }
  return resp::integer(n);
}

std::string cmd_type(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("type");
  touch(c, cmd[1]);
  switch (c.db.type(cmd[1])) {
    case Db::Type::kNone:
      return resp::simple("none");
    case Db::Type::kString:
      return resp::simple("string");
    case Db::Type::kList:
      return resp::simple("list");
  }
  return resp::simple("none");  // unreachable
}

std::string cmd_keys(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("keys");
  std::vector<std::string> encoded;
  for (auto& key : c.db.keys_matching_glob(cmd[1])) {
    if (c.expiry.check_and_expire(key)) continue;  // lazily drop the dead
    encoded.push_back(resp::bulk(key));
  }
  return resp::array(encoded);
}

std::string cmd_dbsize(Ctx& c, const Command& cmd) {
  if (cmd.size() != 1) return wrong_arity("dbsize");
  return resp::integer(static_cast<int64_t>(c.db.size()));
}

std::string cmd_flushall(Ctx& c, const Command& cmd) {
  if (cmd.size() != 1) return wrong_arity("flushall");
  // The frozen ExpiryManager interface has no clear(); disarm per key so no
  // TTL survives the flush (see INTERFACE_PROPOSALS.md).
  for (auto& key : c.db.keys_matching_glob("*")) c.expiry.persist(key);
  c.db.clear();
  return resp::simple("OK");
}

// ---- TTL family ------------------------------------------------------------

std::string expire_generic(Ctx& c, const std::string& name, const Command& cmd,
                           int64_t unit_to_ms) {
  if (cmd.size() != 3) return wrong_arity(name);
  int64_t n = 0;
  if (!parse_i64(cmd[2], n)) return not_an_integer();
  touch(c, cmd[1]);
  if (!c.db.exists(cmd[1])) return resp::integer(0);
  if (n <= 0) {
    // Redis: a non-positive relative expiry deletes the key immediately.
    c.db.del(cmd[1]);
    c.expiry.persist(cmd[1]);
    return resp::integer(1);
  }
  c.expiry.set_expiry(cmd[1], now_ms(c.clock) + n * unit_to_ms);
  return resp::integer(1);
}

std::string cmd_expire(Ctx& c, const Command& cmd) { return expire_generic(c, "expire", cmd, 1000); }
std::string cmd_pexpire(Ctx& c, const Command& cmd) { return expire_generic(c, "pexpire", cmd, 1); }

// The absolute-deadline form. Clients rarely send it; the WAL always does.
// A relative EXPIRE cannot be replayed faithfully — re-evaluating "+10s"
// against the recovery clock silently extends every TTL by the downtime — so
// CP4 rewrites relative expirations into this before they hit the log. Real
// Redis does exactly this when translating commands into the AOF.
std::string cmd_pexpireat(Ctx& c, const Command& cmd) {
  if (cmd.size() != 3) return wrong_arity("pexpireat");
  int64_t at_ms = 0;
  if (!parse_i64(cmd[2], at_ms)) return not_an_integer();
  touch(c, cmd[1]);
  if (!c.db.exists(cmd[1])) return resp::integer(0);
  if (at_ms <= now_ms(c.clock)) {
    // Deadline already behind us — during replay this is a key that died
    // while the process was down. Deleting it here IS the deterministic
    // outcome; leaving it would resurrect it.
    c.db.del(cmd[1]);
    c.expiry.persist(cmd[1]);
    return resp::integer(1);
  }
  c.expiry.set_expiry(cmd[1], at_ms);
  return resp::integer(1);
}

std::string ttl_generic(Ctx& c, const std::string& name, const Command& cmd, bool in_seconds) {
  if (cmd.size() != 2) return wrong_arity(name);
  touch(c, cmd[1]);
  if (!c.db.exists(cmd[1])) return resp::integer(-2);
  auto at = c.expiry.expiry_at(cmd[1]);
  if (!at) return resp::integer(-1);
  int64_t remaining_ms = *at - now_ms(c.clock);
  if (remaining_ms < 0) remaining_ms = 0;
  return resp::integer(in_seconds ? (remaining_ms + 500) / 1000 : remaining_ms);
}

std::string cmd_ttl(Ctx& c, const Command& cmd) { return ttl_generic(c, "ttl", cmd, true); }
std::string cmd_pttl(Ctx& c, const Command& cmd) { return ttl_generic(c, "pttl", cmd, false); }

std::string cmd_persist(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("persist");
  touch(c, cmd[1]);
  if (!c.db.exists(cmd[1])) return resp::integer(0);
  if (!c.expiry.expiry_at(cmd[1])) return resp::integer(0);
  c.expiry.persist(cmd[1]);
  return resp::integer(1);
}

// ---- lists (Tier 2) --------------------------------------------------------

std::string push_generic(Ctx& c, const std::string& name, const Command& cmd, bool left) {
  if (cmd.size() < 3) return wrong_arity(name);
  touch(c, cmd[1]);
  if (c.db.type(cmd[1]) == Db::Type::kString) return wrongtype();
  std::size_t len = 0;
  for (std::size_t i = 2; i < cmd.size(); ++i)
    len = left ? c.db.lpush(cmd[1], cmd[i]) : c.db.rpush(cmd[1], cmd[i]);
  return resp::integer(static_cast<int64_t>(len));
}

std::string cmd_lpush(Ctx& c, const Command& cmd) { return push_generic(c, "lpush", cmd, true); }
std::string cmd_rpush(Ctx& c, const Command& cmd) { return push_generic(c, "rpush", cmd, false); }

std::string pop_generic(Ctx& c, const std::string& name, const Command& cmd, bool left) {
  if (cmd.size() != 2) return wrong_arity(name);
  touch(c, cmd[1]);
  if (c.db.type(cmd[1]) == Db::Type::kString) return wrongtype();
  auto v = left ? c.db.lpop(cmd[1]) : c.db.rpop(cmd[1]);
  if (!v) return resp::null_bulk();
  // Popping the last element removed the key; don't leave its TTL armed.
  if (!c.db.exists(cmd[1])) c.expiry.persist(cmd[1]);
  return resp::bulk(*v);
}

std::string cmd_lpop(Ctx& c, const Command& cmd) { return pop_generic(c, "lpop", cmd, true); }
std::string cmd_rpop(Ctx& c, const Command& cmd) { return pop_generic(c, "rpop", cmd, false); }

std::string cmd_llen(Ctx& c, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("llen");
  touch(c, cmd[1]);
  if (c.db.type(cmd[1]) == Db::Type::kString) return wrongtype();
  return resp::integer(static_cast<int64_t>(c.db.llen(cmd[1])));
}

std::string cmd_lrange(Ctx& c, const Command& cmd) {
  if (cmd.size() != 4) return wrong_arity("lrange");
  int64_t start = 0, stop = 0;
  if (!parse_i64(cmd[2], start) || !parse_i64(cmd[3], stop)) return not_an_integer();
  touch(c, cmd[1]);
  if (c.db.type(cmd[1]) == Db::Type::kString) return wrongtype();
  std::vector<std::string> encoded;
  for (auto& v : c.db.lrange(cmd[1], start, stop)) encoded.push_back(resp::bulk(v));
  return resp::array(encoded);
}

// ---- server / connection ---------------------------------------------------

std::string cmd_ping(Ctx&, const Command& cmd) {
  if (cmd.size() == 1) return resp::simple("PONG");
  if (cmd.size() == 2) return resp::bulk(cmd[1]);
  return wrong_arity("ping");
}

std::string cmd_echo(Ctx&, const Command& cmd) {
  if (cmd.size() != 2) return wrong_arity("echo");
  return resp::bulk(cmd[1]);
}

std::string cmd_command(Ctx&, const Command&) {
  return resp::empty_array();  // stub ok per SPEC §3 (redis-cli sends it on connect)
}

std::string cmd_config(Ctx&, const Command& cmd) {
  // Stub ok per SPEC §3 — just enough for redis-benchmark/redis-cli probes.
  if (cmd.size() >= 2 && upper(cmd[1]) == "GET") {
    if (cmd.size() != 3) return wrong_arity("config|get");
    static const std::unordered_map<std::string, std::string> kKnown = {
        {"save", ""}, {"appendonly", "no"}, {"maxmemory", "0"}};
    auto it = kKnown.find(lower(cmd[2]));
    if (it == kKnown.end()) return resp::empty_array();
    return resp::array({resp::bulk(it->first), resp::bulk(it->second)});
  }
  return resp::error("ERR CONFIG subcommand must be GET");
}

using Handler = std::string (*)(Ctx&, const Command&);

const std::unordered_map<std::string, Handler>& table() {
  static const std::unordered_map<std::string, Handler> kTable = {
      {"PING", cmd_ping},       {"ECHO", cmd_echo},     {"SET", cmd_set},
      {"GET", cmd_get},         {"DEL", cmd_del},       {"EXISTS", cmd_exists},
      {"EXPIRE", cmd_expire},   {"PEXPIRE", cmd_pexpire}, {"TTL", cmd_ttl},
      {"PEXPIREAT", cmd_pexpireat},
      {"PTTL", cmd_pttl},       {"PERSIST", cmd_persist}, {"INCR", cmd_incr},
      {"DECR", cmd_decr},       {"INCRBY", cmd_incrby}, {"TYPE", cmd_type},
      {"FLUSHALL", cmd_flushall}, {"DBSIZE", cmd_dbsize}, {"KEYS", cmd_keys},
      {"CONFIG", cmd_config},   {"COMMAND", cmd_command},
      {"LPUSH", cmd_lpush},     {"RPUSH", cmd_rpush},   {"LPOP", cmd_lpop},
      {"RPOP", cmd_rpop},       {"LRANGE", cmd_lrange}, {"LLEN", cmd_llen},
  };
  return kTable;
}

}  // namespace

CommandDispatcher::CommandDispatcher(Db& db, ExpiryManager& expiry, const Clock& clock)
    : db_(db), expiry_(expiry), clock_(clock) {}

void CommandDispatcher::log_mutation(const std::string& name, const protocol::Command& cmd,
                                     const std::string& reply) {
  // An error reply means nothing changed, so nothing is owed to the log.
  if (reply.empty() || reply[0] == '-') return;

  const auto put = [this](protocol::Command record) { wal_->append(record); };

  if (name == "SET") {
    // "+OK" is the only reply that means a write landed; NX/XX declining
    // answers with a null bulk and must NOT be logged.
    if (reply != "+OK\r\n") return;
    put({"SET", cmd[1], cmd[2]});
    // EX/PX are relative; persist the resolved absolute deadline instead.
    if (auto at = expiry_.expiry_at(cmd[1]))
      put({"PEXPIREAT", cmd[1], std::to_string(*at)});
    return;
  }
  if (name == "EXPIRE" || name == "PEXPIRE" || name == "PEXPIREAT") {
    if (reply != ":1\r\n") return;
    if (auto at = expiry_.expiry_at(cmd[1]))
      put({"PEXPIREAT", cmd[1], std::to_string(*at)});
    else
      put({"DEL", cmd[1]});  // a non-positive TTL deletes outright
    return;
  }
  if (name == "PERSIST") {
    if (reply == ":1\r\n") put({"PERSIST", cmd[1]});
    return;
  }
  if (name == "DEL") {
    if (reply != ":0\r\n") put(cmd);
    return;
  }
  // Deterministic on replay given the same preceding state, so they go in
  // verbatim: the counter reaches the same value by the same route.
  if (name == "INCR" || name == "DECR" || name == "INCRBY" || name == "LPUSH" ||
      name == "RPUSH") {
    if (reply[0] == ':') put(cmd);
    return;
  }
  if (name == "LPOP" || name == "RPOP") {
    if (reply != "$-1\r\n") put(cmd);  // popping an absent list changed nothing
    return;
  }
  if (name == "FLUSHALL") {
    if (reply == "+OK\r\n") put(cmd);
    return;
  }
}

std::string CommandDispatcher::execute(const protocol::Command& cmd) {
  if (cmd.empty()) return resp::error("ERR empty command");
  const std::string name = upper(cmd[0]);

  if (name == "SHUTDOWN") {
    shutdown_ = true;
    return resp::simple("OK");
  }

  auto it = table().find(name);
  if (it == table().end())
    return resp::error("ERR unknown command '" + cmd[0] + "'");

  Ctx ctx{db_, expiry_, clock_};
  std::string reply = it->second(ctx, cmd);

  // CP4 ordering: the record (and its fsync, under --fsync=always) lands here
  // — after the mutation, before the caller can hand the reply to the client.
  if (wal_ != nullptr) log_mutation(name, cmd, reply);
  return reply;
}

}  // namespace hermit::core

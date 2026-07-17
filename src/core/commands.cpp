#include "core/commands.h"

#include <algorithm>
#include <cctype>

#include "protocol/resp_writer.h"

namespace hermit::core {
namespace {

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

std::string wrong_arity(const std::string& name_lower) {
  return resp::error("ERR wrong number of arguments for '" + name_lower + "' command");
}

}  // namespace

CommandDispatcher::CommandDispatcher(Db& db, ExpiryManager& expiry, const Clock& clock)
    : db_(db), expiry_(expiry), clock_(clock) {}

std::string CommandDispatcher::execute(const protocol::Command& cmd) {
  (void)db_;
  (void)expiry_;
  (void)clock_;
  (void)wal_;
  if (cmd.empty()) return resp::error("ERR empty command");
  const std::string name = upper(cmd[0]);

  // The three replies needed so redis-cli can connect and talk to a bare CP2
  // loop (redis-cli sends COMMAND on connect). Tier 1 proper lands in M3.
  if (name == "PING") {
    if (cmd.size() == 1) return resp::simple("PONG");
    if (cmd.size() == 2) return resp::bulk(cmd[1]);
    return wrong_arity("ping");
  }
  if (name == "ECHO") {
    if (cmd.size() != 2) return wrong_arity("echo");
    return resp::bulk(cmd[1]);
  }
  if (name == "COMMAND") {
    return resp::empty_array();  // stub ok per SPEC §3
  }
  if (name == "SHUTDOWN") {
    shutdown_ = true;
    return resp::simple("OK");
  }

  return resp::error("ERR unknown command '" + cmd[0] +
                     "' (hermitdb M0 scaffold: Tier 1 lands in M3)");
}

}  // namespace hermit::core

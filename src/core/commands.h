#pragma once
// Command dispatch + argument validation. [Claude Code] — implemented at M3.
// At M0 only PING/ECHO/COMMAND answer (so redis-cli can connect the moment
// CP2 works); every other command replies "-ERR ... not implemented".

#include <string>

#include "core/db.h"
#include "core/expiry.h"
#include "protocol/resp_parser.h"
#include "util/clock.h"

namespace hermit::persist {
class Wal;  // attached at M5; forward-declared to keep core/ independent
}

namespace hermit::core {

class CommandDispatcher {
 public:
  CommandDispatcher(Db& db, ExpiryManager& expiry, const Clock& clock);

  // Attached at M5. When non-null, every mutating command is appended
  // post-validation (exact ordering vs. reply: CP4's call to make).
  void set_wal(persist::Wal* wal) { wal_ = wal; }

  // Executes one client command, returns the RESP-encoded reply.
  std::string execute(const protocol::Command& cmd);

  // Set by SHUTDOWN; the event loop polls this after each command.
  bool shutdown_requested() const { return shutdown_; }

 private:
  Db& db_;
  ExpiryManager& expiry_;
  const Clock& clock_;
  persist::Wal* wal_ = nullptr;
  bool shutdown_ = false;
};

}  // namespace hermit::core

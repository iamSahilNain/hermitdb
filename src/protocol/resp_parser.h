#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hermit::protocol {

// A single client command as argv, e.g. {"SET", "key", "value"}.
using Command = std::vector<std::string>;

enum class ParseStatus {
  // Progress was made. Zero or more complete commands were extracted; any
  // trailing partial input is retained inside the parser for the next feed().
  kOk,
  // Unrecoverable framing violation. The caller must send `-ERR <error>` and
  // close the connection; the parser's state is undefined afterwards.
  kProtocolError,
  // Stub state — CHECKPOINT 1 not yet implemented.
  kNotImplemented,
};

struct ParseResult {
  ParseStatus status = ParseStatus::kNotImplemented;
  std::vector<Command> commands;
  std::string error;  // for kProtocolError: message without leading '-' or trailing CRLF
};

// ============================= CHECKPOINT 1 =================================
// Incremental RESP2 command parser. Study sheet: checkpoints/CP1.md
//
// CONTRACT
//  - feed() consumes an arbitrary byte chunk. TCP provides NO message
//    boundaries: a command may arrive one byte per call, or fifty commands
//    may arrive in a single call (pipelining). Both must work.
//  - Complete commands are returned in arrival order in ParseResult::commands.
//    Partial trailing input is buffered internally across calls.
//  - Two client framings must be accepted:
//      1. Array-of-bulk-strings: *<n>\r\n then n of $<len>\r\n<bytes>\r\n
//         (bulk payloads are binary-safe — they may contain \r\n).
//      2. Inline commands: a bare line "PING\r\n" or "SET k v\r\n",
//         whitespace-separated (redis-cli uses these). An empty inline line
//         ("\r\n" alone) produces no command and is not an error.
//  - "*0\r\n" is a legal frame producing no command.
//  - Defense: any declared array length, bulk length, or accumulated frame
//    size exceeding max_frame_bytes ⇒ kProtocolError (the `*1000000000\r\n`
//    memory-blowup attack must die here, before allocation).
//  - Malformed input (non-numeric lengths, negative bulk length inside a
//    command, a '$' header where one is required but absent, bulk payload not
//    terminated by \r\n) ⇒ kProtocolError with a human-readable message.
//  - After kProtocolError the caller closes the connection; feed() will not
//    be called again on this instance.
// ============================================================================
class RespParser {
 public:
  explicit RespParser(std::size_t max_frame_bytes);

  ParseResult feed(std::string_view bytes);

 private:
  // ==== CHECKPOINT 1: YOUR CODE ====
  // Your incremental state lives here: buffered bytes, cursor, the partially
  // decoded frame (declared argc, args collected so far, expected bulk
  // length, ...) — whatever your design needs.
  std::size_t max_frame_bytes_;
  // ==== END CHECKPOINT 1 ====
};

}  // namespace hermit::protocol

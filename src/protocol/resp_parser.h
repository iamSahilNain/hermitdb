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
  // A three-state machine, not recursive descent: recursion would need the
  // whole frame resident to unwind, and the whole point is that we never have
  // the whole frame. Each state consumes what it can and returns; the residue
  // stays in buf_ for the next feed().
  enum class State {
    kLine,       // between frames: expecting "*<n>" or an inline command line
    kBulkHead,   // inside an array: expecting "$<len>"
    kBulkBody,   // expecting <len> payload bytes + CRLF
  };

  // Consumes one CRLF-terminated line at cursor_ (the trailing \r is stripped).
  // Returns false when no complete line is buffered yet.
  bool take_line(std::string_view& line);
  // Splits an inline command on whitespace. Empty line => no command.
  static void split_inline(std::string_view line, std::vector<std::string>& out);

  std::size_t max_frame_bytes_;

  std::string buf_;          // unconsumed bytes, oldest first
  std::size_t cursor_ = 0;   // parse position within buf_
  State state_ = State::kLine;
  int64_t pending_argc_ = 0;     // args still owed by the current array frame
  int64_t bulk_len_ = 0;         // payload length awaited in kBulkBody
  std::vector<std::string> args_;  // args decoded so far for the current frame
  // ==== END CHECKPOINT 1 ====
};

}  // namespace hermit::protocol

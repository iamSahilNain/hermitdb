#include "protocol/resp_parser.h"

#include <charconv>
#include <cstdint>

namespace hermit::protocol {
namespace {

// Independent of max_frame_bytes_: a 64 MiB frame cap would otherwise permit
// an argv of 64M empty strings, which costs ~2 GiB of std::string headers
// before a single payload byte arrives. Real Redis caps multibulk at 1M.
constexpr int64_t kMaxArgs = 1024 * 1024;

bool parse_i64(std::string_view s, int64_t& out) {
  if (s.empty()) return false;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, out);
  return ec == std::errc() && ptr == last;
}

ParseResult fail(std::string msg) {
  ParseResult r;
  r.status = ParseStatus::kProtocolError;
  r.error = std::move(msg);
  return r;
}

bool is_space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

RespParser::RespParser(std::size_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {}

bool RespParser::take_line(std::string_view& line) {
  const std::size_t nl = buf_.find('\n', cursor_);
  if (nl == std::string::npos) return false;
  std::size_t end = nl;
  if (end > cursor_ && buf_[end - 1] == '\r') --end;  // tolerate bare \n
  line = std::string_view(buf_).substr(cursor_, end - cursor_);
  cursor_ = nl + 1;
  return true;
}

void RespParser::split_inline(std::string_view line, std::vector<std::string>& out) {
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && is_space(line[i])) ++i;
    if (i >= line.size()) break;
    const std::size_t start = i;
    while (i < line.size() && !is_space(line[i])) ++i;
    out.emplace_back(line.substr(start, i - start));
  }
}

ParseResult RespParser::feed(std::string_view bytes) {
  ParseResult result;
  result.status = ParseStatus::kOk;

  buf_.append(bytes.data(), bytes.size());

  for (;;) {
    if (state_ == State::kLine) {
      std::string_view line;
      if (!take_line(line)) break;  // incomplete — wait for more bytes
      if (line.empty()) continue;   // bare CRLF: legal, produces no command

      if (line[0] == '*') {
        int64_t n = 0;
        if (!parse_i64(line.substr(1), n))
          return fail("ERR Protocol error: invalid multibulk length");
        // n <= 0 ("*0", and the null array "*-1") is a complete frame that
        // yields no command — same as real Redis, which resets and moves on.
        if (n <= 0) continue;
        if (n > kMaxArgs || static_cast<uint64_t>(n) > max_frame_bytes_)
          return fail("ERR Protocol error: invalid multibulk length");
        pending_argc_ = n;
        args_.clear();
        state_ = State::kBulkHead;
        continue;
      }

      // Inline framing — what redis-cli sends when you type at its prompt.
      Command cmd;
      split_inline(line, cmd);
      if (!cmd.empty()) result.commands.push_back(std::move(cmd));
      continue;
    }

    if (state_ == State::kBulkHead) {
      if (static_cast<int64_t>(args_.size()) == pending_argc_) {
        result.commands.push_back(std::move(args_));
        args_.clear();
        state_ = State::kLine;
        continue;
      }
      std::string_view line;
      if (!take_line(line)) break;
      if (line.empty() || line[0] != '$')
        return fail("ERR Protocol error: expected '$', got something else");
      int64_t len = 0;
      if (!parse_i64(line.substr(1), len))
        return fail("ERR Protocol error: invalid bulk length");
      // A null bulk ($-1) is a valid *reply* element but never a command
      // argument — arguments are always concrete byte strings.
      // +2 for the CRLF: a length that only fits without its terminator would
      // pass here and then wedge forever against the buffered-size check.
      if (len < 0 || static_cast<uint64_t>(len) + 2 > max_frame_bytes_)
        return fail("ERR Protocol error: invalid bulk length");
      bulk_len_ = len;
      state_ = State::kBulkBody;
      continue;
    }

    // kBulkBody: length-framed, so the payload is binary-safe — a value
    // containing \r\n is data here, never a delimiter.
    const std::size_t need = static_cast<std::size_t>(bulk_len_) + 2;
    if (buf_.size() - cursor_ < need) break;
    const std::size_t term = cursor_ + static_cast<std::size_t>(bulk_len_);
    if (buf_[term] != '\r' || buf_[term + 1] != '\n')
      return fail("ERR Protocol error: bulk payload not terminated by CRLF");
    args_.emplace_back(buf_, cursor_, static_cast<std::size_t>(bulk_len_));
    cursor_ = term + 2;
    state_ = State::kBulkHead;
  }

  // We only get here by running out of bytes mid-frame. Whatever is still
  // unconsumed is ONE incomplete frame, so it is the thing to size-check:
  // this is where "*1000000000\r\n"'s little brother — a never-terminated
  // inline line — dies, before it can eat the heap.
  const std::size_t buffered = buf_.size() - cursor_;
  if (buffered > max_frame_bytes_)
    return fail("ERR Protocol error: too big inline request");

  // Reclaim the consumed prefix so a long-lived connection's buffer tracks
  // the in-flight frame, not the session's history.
  if (cursor_ > 0) {
    buf_.erase(0, cursor_);
    cursor_ = 0;
  }
  return result;
}

}  // namespace hermit::protocol

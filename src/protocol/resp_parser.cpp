#include "protocol/resp_parser.h"

namespace hermit::protocol {

RespParser::RespParser(std::size_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {}

ParseResult RespParser::feed(std::string_view bytes) {
  (void)bytes;
  (void)max_frame_bytes_;
  // ==== CHECKPOINT 1: YOUR CODE ====
  // Stub: reports kNotImplemented so every caller (and every test in
  // tests/unit/cp1_resp_parser_test.cpp) fails loudly instead of silently.
  return {};
  // ==== END CHECKPOINT 1 ====
}

}  // namespace hermit::protocol

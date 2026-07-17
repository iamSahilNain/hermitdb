#include "protocol/resp_writer.h"

namespace hermit::resp {

std::string simple(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 3);
  out += '+';
  out += s;
  out += "\r\n";
  return out;
}

std::string error(std::string_view msg) {
  std::string out;
  out.reserve(msg.size() + 3);
  out += '-';
  out += msg;
  out += "\r\n";
  return out;
}

std::string integer(int64_t n) { return ":" + std::to_string(n) + "\r\n"; }

std::string bulk(std::string_view payload) {
  std::string out;
  out.reserve(payload.size() + 16);
  out += '$';
  out += std::to_string(payload.size());
  out += "\r\n";
  out += payload;
  out += "\r\n";
  return out;
}

std::string null_bulk() { return "$-1\r\n"; }

std::string array(const std::vector<std::string>& encoded_elements) {
  std::string out = "*" + std::to_string(encoded_elements.size()) + "\r\n";
  for (const auto& e : encoded_elements) out += e;
  return out;
}

std::string null_array() { return "*-1\r\n"; }

std::string empty_array() { return "*0\r\n"; }

}  // namespace hermit::resp

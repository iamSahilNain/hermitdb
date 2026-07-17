#pragma once
// RESP2 reply serialization. [Claude Code] — plumbing, not a checkpoint.
// Every function returns the fully framed wire bytes, ready to queue on a
// connection's output buffer.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hermit::resp {

// +<s>\r\n           e.g. simple("OK") -> "+OK\r\n"
std::string simple(std::string_view s);

// -<msg>\r\n         e.g. error("ERR unknown command 'FOO'")
std::string error(std::string_view msg);

// :<n>\r\n
std::string integer(int64_t n);

// $<len>\r\n<bytes>\r\n — binary-safe; payload may contain \r\n.
std::string bulk(std::string_view payload);

// $-1\r\n            (Redis "nil")
std::string null_bulk();

// *<n>\r\n<elem0><elem1>... — elements must already be RESP-encoded.
std::string array(const std::vector<std::string>& encoded_elements);

// *-1\r\n
std::string null_array();

// *0\r\n
std::string empty_array();

}  // namespace hermit::resp

#pragma once
#include <string>
#include <string_view>
#include "http/fixed_buffer.hpp"

namespace http {

// Status text for common HTTP status codes
std::string_view StatusText(int code);

// Build HTTP response headers only (no body).
// Returns: "HTTP/1.1 {status}\r\nContent-Type: ...\r\nContent-Length: ...\r\n\r\n"
std::string BuildHeader(int code, const std::string& mime, size_t body_size);

// Build headers into a FixedBuffer (zero heap allocation during construction).
// The result is available via buf.View().
void BuildHeaderTo(FixedBuffer& buf, int code, std::string_view mime, size_t body_size);

// Build a complete HTTP response string (headers + body)
std::string MakeResponse(int code, const std::string& mime, const std::string& body);

// Build a complete HTTP error response (text/html body)
std::string MakeError(int code);

} // namespace http

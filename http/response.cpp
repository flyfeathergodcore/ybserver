#include "http/response.hpp"

namespace http {

std::string_view StatusText(int code)
{
    switch (code) {
        case 200: return "200 OK";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 426: return "426 Upgrade Required";
        case 501: return "501 Not Implemented";
        default:  return "500 Internal Server Error";
    }
}

void BuildHeaderTo(FixedBuffer& buf, int code,
                    std::string_view mime, size_t body_size)
{
    buf.Write("HTTP/1.1 ");
    buf.Write(StatusText(code));
    buf.WriteCRLF();
    buf.WriteContentType(mime);
    buf.WriteContentLength(body_size);
    buf.WriteKeepAlive();
    buf.WriteCRLF();
}

std::string BuildHeader(int code, const std::string& mime, size_t body_size)
{
    FixedBuffer buf;
    BuildHeaderTo(buf, code, mime, body_size);
    return std::string(buf.View());
}

std::string MakeResponse(int code, const std::string& mime, const std::string& body)
{
    return BuildHeader(code, mime, body.size()) + body;
}

std::string MakeError(int code)
{
    std::string_view text =
        (code == 400) ? "Bad Request" :
        (code == 403) ? "Forbidden" :
        (code == 404) ? "Not Found" :
        (code == 501) ? "Not Implemented" :
                        "Error";
    std::string body = "<h1>" + std::to_string(code) + " "
                       + std::string(text) + "</h1>";
    return MakeResponse(code, "text/html", body);
}

} // namespace http

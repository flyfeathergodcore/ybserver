// ═══════════════════════════════════════════════════════════════════
// H2 WebSocket (RFC 8441) echo test
//
// Extended CONNECT → /echo → send "hello" → expect "hello" back.
// ═══════════════════════════════════════════════════════════════════

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <nghttp2/nghttp2.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

using asio::ip::tcp;

struct TestContext {
    nghttp2_session* session = nullptr;
    bool got_200 = false;
    bool headers_end = false;
    bool stream_closed = false;
    std::string recv_data;
    std::vector<uint8_t> output;
    int32_t stream_id = -1;
    bool data_sent = false;
};

static ssize_t cb_send(nghttp2_session*, const uint8_t* data, size_t len,
                        int, void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    ctx->output.insert(ctx->output.end(), data, data + len);
    return static_cast<ssize_t>(len);
}

static int cb_on_header(nghttp2_session*, const nghttp2_frame*,
                         const uint8_t* name, size_t namelen,
                         const uint8_t* value, size_t valuelen,
                         uint8_t, void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    std::string_view n((const char*)name, namelen);
    std::string_view v((const char*)value, valuelen);
    printf("  HEADER: %.*s = %.*s\n", (int)namelen, name, (int)valuelen, value);
    if (n == ":status" && v == "200")
        ctx->got_200 = true;
    return 0;
}

static int cb_on_frame_recv(nghttp2_session*, const nghttp2_frame* frame,
                             void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS)
        ctx->headers_end = (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) != 0;
    return 0;
}

static int cb_on_data_chunk_recv(nghttp2_session*, uint8_t, int32_t,
                                  const uint8_t* data, size_t len,
                                  void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    ctx->recv_data.append((const char*)data, len);
    printf("  DATA[%zu]: %.*s\n", len, (int)len, data);
    return 0;
}

static int cb_on_stream_close(nghttp2_session*, int32_t, uint32_t ec,
                               void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    ctx->stream_closed = true;
    printf("[test] stream closed (code=%u)\n", ec);
    return 0;
}

int main() {
    asio::io_context io;
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv13_client);
    ssl_ctx.set_verify_mode(asio::ssl::verify_none);

    tcp::resolver r(io);
    auto eps = r.resolve("127.0.0.1", "8443");
    asio::ssl::stream<tcp::socket> s(io, ssl_ctx);

    asio::error_code ec;
    asio::connect(s.next_layer(), eps, ec);
    if (ec) { std::cerr << "connect: " << ec.message() << std::endl; return 1; }
    SSL_set_alpn_protos(s.native_handle(), (const unsigned char*)"\x02h2", 3);
    s.handshake(asio::ssl::stream_base::client, ec);
    if (ec) { std::cerr << "tls: " << ec.message() << std::endl; return 1; }

    printf("[test] H2 WebSocket echo test\n\n");

    TestContext ctx;
    nghttp2_session_callbacks* cb;
    nghttp2_session_callbacks_new(&cb);
    nghttp2_session_callbacks_set_send_callback(cb, cb_send);
    nghttp2_session_callbacks_set_on_header_callback(cb, cb_on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, cb_on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, cb_on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, cb_on_stream_close);
    nghttp2_session_client_new(&ctx.session, cb, &ctx);
    nghttp2_session_callbacks_del(cb);

    auto flush = [&] {
        if (ctx.output.empty()) return;
        asio::write(s, asio::buffer(ctx.output));
        ctx.output.clear();
    };

    // ── Preface ──
    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1}};
    nghttp2_submit_settings(ctx.session, NGHTTP2_FLAG_NONE, settings, 1);
    nghttp2_session_send(ctx.session);
    flush();

    // ── Read server SETTINGS ──
    std::array<uint8_t, 4096> buf;
    auto n = s.read_some(asio::buffer(buf), ec);
    if (n > 0) nghttp2_session_mem_recv(ctx.session, buf.data(), n);

    // ── Extended CONNECT to /echo ──
    std::vector<nghttp2_nv> nv;
    auto add = [&](const char* n, const char* v) {
        nv.push_back({(uint8_t*)n, (uint8_t*)v,
                       (uint16_t)strlen(n), (uint16_t)strlen(v),
                       NGHTTP2_NV_FLAG_NONE});
    };
    add(":method", "CONNECT");
    add(":protocol", "websocket");
    add(":path", "/echo");
    add(":authority", "localhost");
    add("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
    add("sec-websocket-version", "13");
    add("origin", "https://localhost");

    ctx.stream_id = nghttp2_submit_headers(ctx.session, NGHTTP2_FLAG_END_HEADERS,
                                            -1, nullptr, nv.data(), nv.size(), nullptr);
    printf("[test] stream %d (Extended CONNECT /echo)\n", ctx.stream_id);
    nghttp2_session_send(ctx.session);
    flush();

    // ── Poll until 200 received ──
    while (!ctx.got_200 && !ctx.stream_closed) {
        n = s.read_some(asio::buffer(buf), ec);
        if (n > 0) nghttp2_session_mem_recv(ctx.session, buf.data(), n);
        nghttp2_session_send(ctx.session);
        flush();
        if (!ctx.got_200 && !ctx.stream_closed)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!ctx.got_200) {
        printf("[test] did not get 200 response\n");
        return 1;
    }
    printf("[test] got 200, sending \"hello\"...\n");

    // ── Send DATA("hello") with END_STREAM ──
    const char* hello = "hello";
    nghttp2_data_provider dp;
    dp.source.ptr = (void*)hello;
    dp.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf,
                           size_t len, uint32_t* flags,
                           nghttp2_data_source* src, void*) -> ssize_t {
        const char* msg = (const char*)src->ptr;
        size_t msglen = strlen(msg);
        size_t copy = std::min(len, msglen);
        memcpy(buf, msg, copy);
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        return (ssize_t)copy;
    };
    nghttp2_submit_data(ctx.session, NGHTTP2_FLAG_END_STREAM,
                         ctx.stream_id, &dp);
    nghttp2_session_send(ctx.session);
    flush();

    // ── Poll for response + stream close ──
    for (int round = 0; round < 100 && !ctx.stream_closed; round++) {
        n = s.read_some(asio::buffer(buf), ec);
        if (n > 0) nghttp2_session_mem_recv(ctx.session, buf.data(), n);
        nghttp2_session_send(ctx.session);
        flush();
        if (!ctx.stream_closed)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ── Results ──
    bool pass = ctx.got_200 && ctx.recv_data == "hello" && ctx.stream_closed;
    printf("\n════════════════════════════════\n");
    printf("  Got 200:       %s\n", ctx.got_200 ? "YES ✓" : "NO ✗");
    printf("  Echo match:    %s\n", ctx.recv_data == "hello" ? "YES ✓" : "NO ✗");
    printf("  Recv data:     \"%s\" (%zu bytes)\n", ctx.recv_data.c_str(), ctx.recv_data.size());
    printf("  Stream closed: %s\n", ctx.stream_closed ? "YES" : "NO");
    printf("  Status:        %s\n", pass ? "PASS ✓" : "FAIL ✗");
    printf("════════════════════════════════\n");

    nghttp2_session_del(ctx.session);
    return pass ? 0 : 1;
}

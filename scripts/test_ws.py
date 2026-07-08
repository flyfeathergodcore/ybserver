#!/usr/bin/env python3
"""WebSocket 升级测试 — 验证 101 握手 + 回显"""
import asyncio, ssl, sys, hashlib, base64
import http.client, ssl as ssl_mod

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

async def test_ws():
    uri = "wss://localhost:8443/echo"
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE

    # ── 低层 HTTP 请求：验证服务端 Sec-WebSocket-Accept 计算正确 ──
    ctx = ssl_mod.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl_mod.CERT_NONE
    conn = http.client.HTTPSConnection("localhost", 8443, context=ctx)
    my_key = base64.b64encode(b"test-handshake").decode()
    conn.request("GET", "/echo",
        headers={
            "Upgrade": "websocket",
            "Connection": "Upgrade",
            "Sec-WebSocket-Key": my_key,
            "Sec-WebSocket-Version": "13",
        })
    resp = conn.getresponse()
    raw_headers = resp.getheaders()
    server_accept = ""
    for h, v in raw_headers:
        if h.lower() == "sec-websocket-accept":
            server_accept = v
            break
    expected_accept = base64.b64encode(hashlib.sha1(
        (my_key + WS_GUID).encode()
    ).digest()).decode()
    if server_accept != expected_accept:
        print(f"  ✗ Accept 不匹配")
        print(f"    key:      {my_key}")
        print(f"    服务端:   '{server_accept}'")
        print(f"    预期:     '{expected_accept}'")
        return False
    conn.close()

    # ── 完整 WebSocket 握手 + 通信（使用 websockets 库） ──
    import websockets
    try:
        async with websockets.connect(uri, ssl=ssl_ctx, max_size=2**20, close_timeout=5) as ws:
            print(f"  ✓ WebSocket 连接成功 (101 握手完成)")
            await ws.send("hello")
            echo = await ws.recv()
            if echo == "hello":
                print(f"  ✓ 回显正确: {echo}")
            else:
                print(f"  ✗ 回显不匹配: {echo}")
                return False
            await ws.close()
            print(f"  ✓ 正常关闭")
            return True
    except Exception as e:
        print(f"  ✗ 连接失败: {e}")
        return False

if __name__ == "__main__":
    ok = asyncio.run(test_ws())
    sys.exit(0 if ok else 1)

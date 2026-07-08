#!/usr/bin/env python3
"""WebSocket 测试 — 用 websocket-client (RFC 6455 兼容)"""
import websocket
import ssl
import sys

url = "wss://localhost:8443/echo"
ssl_ctx = {"cert_reqs": ssl.CERT_NONE, "check_hostname": False}

try:
    ws = websocket.create_connection(url, sslopt=ssl_ctx, timeout=5)
    print(f"  ✓ WebSocket 连接成功 (101 握手完成)")
    ws.send("hello")
    echo = ws.recv()
    if echo == "hello":
        print(f"  ✓ 回显正确: {echo}")
    else:
        print(f"  ✗ 回显不匹配: {echo}")
        sys.exit(1)
    ws.close()
    print(f"  ✓ 正常关闭")
    sys.exit(0)
except Exception as e:
    print(f"  ✗ 连接失败: {e}")
    sys.exit(1)

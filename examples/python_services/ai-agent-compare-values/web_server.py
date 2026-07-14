"""
购物导购 Agent Web 服务 — 端口 8080（HTTP）

通过 run.py 启动时共享 gRPC 服务的会话存储。
也可单独启动（python web_server.py）用于调试。

GET  /              — 聊天界面
POST /api/chat      — 聊天（SSE 流式，仅供反向代理兼容）
POST /api/reset     — 重置会话
"""
import json, os, sys, time, urllib.request, urllib.parse, re
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

# ── 会话管理（由 run.py 注入，独立启动时自建）──
_sessions: dict[str, dict] = {}
_get_or_create_session = None
_llm = None  # LLMClient 实例


def _ensure_initialized():
    """独立启动时自建 MCP 服务和会话管理（延迟初始化）"""
    global _get_or_create_session, _sessions, _llm
    if _get_or_create_session is not None:
        return
    from mcp_servers.mcp_setup import create_mcp_services, create_session_factory
    from core.config import load_config
    cfg = load_config()
    guide_mcp, product_mcp, _llm = create_mcp_services(cfg)
    _sessions, _get_or_create_session = create_session_factory(_llm, guide_mcp, product_mcp, cfg)


def start_http_server(session_factory, sessions_dict, llm_client=None):
    """由 run.py 调用，注入共享会话存储"""
    global _get_or_create_session, _sessions, _llm
    _get_or_create_session = session_factory
    _sessions = sessions_dict
    if llm_client is not None:
        _llm = llm_client

    port = int(os.getenv("PORT", "8080"))
    print(f"[http] 服务启动: http://0.0.0.0:{port}", flush=True)
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()



# ── HTML ──
_HTML_CACHE = None
def _load_html():
    global _HTML_CACHE
    if _HTML_CACHE: return _HTML_CACHE
    paths = [os.path.join(ROOT, "..", "..", "..", "www", "shopping.html"),
             os.path.join(ROOT, "..", "www", "shopping.html"),
             "/www/shopping.html"]
    for p in paths:
        if os.path.exists(p):
            with open(p) as f: _HTML_CACHE = f.read()
            return _HTML_CACHE
    return "<h1>shopping.html not found</h1>"


def _norm(path):
    if path.startswith('/shopping'):
        path = '/' + path[len('/shopping/'):] if len(path) > len('/shopping/') else '/'
    return path


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        _ensure_initialized()
        p = _norm(self.path)
        if p in ('', '/', '/index.html'):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(_load_html().encode())
        elif self.path == "/health":
            self.send_response(200); self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        _ensure_initialized()
        cl = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(cl)) if cl > 0 else {}
        p = _norm(self.path)
        if p == "/api/chat":
            self._handle_chat_stream(body)
        elif p == "/api/reset":
            sid = body.get("session_id", "")
            if sid in _sessions: del _sessions[sid]
            data = json.dumps({"ok": True}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404); self.end_headers(); return

    def _handle_chat_stream(self, body):
        msg = body.get("message", "")
        sid = body.get("session_id", "")
        print(f"[request] POST /api/chat  sid={sid[:20] if sid else 'new'}  msg={msg[:40]}...", flush=True)
        sid = sid or f"sess_{int(time.time())}"
        sess = _get_or_create_session(sid)
        guide = sess["guide"]
        product = sess["product"]
        t0 = time.time()

        # ── 发送 SSE header ──
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        meta = json.dumps({"session_id": sid, "stage": sess["stage"], "ready": False}, ensure_ascii=False)
        self.wfile.write(f"data: {meta}\n\n".encode())
        self.wfile.flush()

        try:
            if sess["stage"] == "guide":
                result = guide.analyze_l1([{"role": "user", "content": msg}])

                if result.get("final") is True:
                    print(f"[request] final detected → switch to product stage", flush=True)
                    sess["stage"] = "product"
                    sess["last_action"] = "final"
                    result = product.product_main_loop(
                        session_id=sid, first_call=True)

            elif sess["stage"] == "product":
                sess["last_action"] = "product_message"
                product_history = sess.get("product_history", [])
                result = product.product_main_loop(
                    session_id=sid,
                    user_message=msg,
                    history=product_history,
                )
                sess["product_history"] = product._product_history

        except Exception as e:
            print(f"[agent] _handle_chat_stream 失败: {e}", flush=True)
            err = json.dumps({"error": str(e)}, ensure_ascii=False)
            self.wfile.write(f"data: {err}\n\n".encode())
            self.wfile.flush()
            return

        t1 = time.time()
        reply = result.get("reply", "")
        candidates = result.get("candidates", [])
        print(f"[request] ← reply  {len(reply)}B  {((t1-t0)*1000):.0f}ms  candidates={len(candidates)}", flush=True)

        # 分批推送回复文本
        if reply:
            chunk_size = 4
            for i in range(0, len(reply), chunk_size):
                chunk = json.dumps({"token": reply[i:i+chunk_size]}, ensure_ascii=False)
                self.wfile.write(f"data: {chunk}\n\n".encode())
                self.wfile.flush()

        # 候选产品
        done_candidates = []
        for c in candidates[:5]:
            done_candidates.append({
                "name": str(c.get("name", c.get("skuName", ""))),
                "price": str(c.get("price", c.get("current_price", "0"))),
                "rating": str(c.get("rating", "0")),
            })

        done_data = json.dumps({
            "done": True, "full_text": reply or "抱歉，我没有理解你的需求，请重新说一遍。",
            "stage": sess["stage"], "ready": True, "candidates": done_candidates,
        }, ensure_ascii=False)
        self.wfile.write(f"data: {done_data}\n\n".encode())
        self.wfile.flush()

    def log_message(self, fmt, *args):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {args[0]}", flush=True)


if __name__ == "__main__":
    _ensure_initialized()
    port = int(os.getenv("PORT", "8080"))
    print(f"[http] 独立服务启动: http://0.0.0.0:{port}", flush=True)
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()

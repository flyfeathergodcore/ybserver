"""
购物导购 Agent gRPC 服务端 — 端口 50054

由 C++ shopping_handler 通过 gRPC 调用（替代原来的 HTTP 调用）。

通过 run.py 启动时共享 HTTP 服务的会话存储。
也可单独启动（python grpc_server.py）用于调试。

RPCs:
  ChatStream(ShoppingRequest) → stream ShoppingEvent  — 流式对话
  ResetSession(ResetRequest)  → ResetResponse        — 重置会话
"""
import json, os, sys, time, re, asyncio

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

import grpc
from grpc.aio import server as grpc_server
import shopping_pb2, shopping_pb2_grpc

# ── 共享资源（由 run.py 注入，独立启动时自建）──
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


async def start_grpc_server(session_factory, sessions_dict=None, llm_client=None):
    """由 run.py 调用，注入共享会话存储和 LLM 客户端"""
    global _get_or_create_session, _sessions, _llm
    _get_or_create_session = session_factory
    if sessions_dict is not None:
        _sessions = sessions_dict
    if llm_client is not None:
        _llm = llm_client

    port = int(os.getenv("GRPC_PORT", "50054"))
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    shopping_pb2_grpc.add_ShoppingServiceServicer_to_server(
        ShoppingServicer(), server)
    print(f"[grpc] 服务启动: :{port}", flush=True)
    await server.start()
    await server.wait_for_termination()


def _parse_tool_request(raw: str):
    """检测 LLM 输出是否为 tool 请求"""
    try:
        m = re.search(r'\{[\s\S]*"tool"\s*:\s*true[\s\S]*\}', raw)
        if not m:
            return None
        obj = json.loads(m.group())
        if obj.get("tool") is True:
            return (obj.get("tool_name", ""), obj.get("kwargs", ""))
    except Exception:
        pass
    return None


class ShoppingServicer(shopping_pb2_grpc.ShoppingServiceServicer):
    """gRPC 服务实现"""

    async def ChatStream(self, request, context):
        _ensure_initialized()
        msg = request.message
        sid = request.session_id or f"sess_{int(time.time())}"
        print(f"[request] ChatStream  sid={sid[:20]}  msg={msg[:40]}...", flush=True)

        sess = _get_or_create_session(sid)
        guide = sess["guide"]
        t0 = time.time()

        yield shopping_pb2.ShoppingEvent(meta=shopping_pb2.MetaEvent(
            session_id=sid, stage="guide", ready=False))

        # ── 使用 analyze_l1 处理完整的 tool 请求流程 ──
        # analyze_l1 内部会：
        # 1. 调用 LLM 检查是否需要加载品类 tool
        # 2. 如果需要→调 MCP→再调 LLM
        # 3. 返回 {"reply": "追问"} 或 {"reply": "", "intent": {...}, "ready": True}
        try:
            result = guide.analyze_l1([{"role": "user", "content": msg}])
        except Exception as e:
            print(f"[agent] analyze_l1 失败: {e}", flush=True)
            yield shopping_pb2.ShoppingEvent(
                error=shopping_pb2.ErrorEvent(message=str(e)))
            return

        t1 = time.time()
        elapsed_ms = int((t1 - t0) * 1000)

        reply = result.get("reply", "")
        product_interacted = result.get("product_interacted", False)
        t1 = time.time()
        elapsed_ms = int((t1 - t0) * 1000)
        print(f"[request] ← reply  {len(reply)}B  {elapsed_ms}ms  product={product_interacted}", flush=True)

        # 组装候选产品列表（用于前端展示）
        done_candidates = []
        if product_interacted:
            for c in guide._recommended_products[:5]:
                done_candidates.append(shopping_pb2.Candidate(
                    name=str(c.get("name", "")),
                    price=str(c.get("price", "")),
                    rating=str(c.get("rating", "")),
                ))

        if reply:
            yield shopping_pb2.ShoppingEvent(
                token=shopping_pb2.TokenEvent(token=reply))
            yield shopping_pb2.ShoppingEvent(done=shopping_pb2.DoneEvent(
                full_text=reply, candidates=done_candidates))
        else:
            yield shopping_pb2.ShoppingEvent(done=shopping_pb2.DoneEvent(
                full_text="抱歉，我没有理解你的需求，请重新说一遍。"))

    async def ResetSession(self, request, context):
        _ensure_initialized()
        sid = request.session_id
        if sid in _sessions:
            del _sessions[sid]
        print(f"[reset] session={sid[:20] if sid else 'all'}", flush=True)
        return shopping_pb2.ResetResponse(ok=True)


if __name__ == "__main__":
    _ensure_initialized()
    asyncio.run(start_grpc_server(_get_or_create_session))

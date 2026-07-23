"""
购物导购 Agent gRPC 服务端 — 端口 50054

由 C++ shopping_handler 通过 gRPC 调用（替代原来的 HTTP 调用）。

通过 run.py 启动时共享 HTTP 服务的会话存储。
也可单独启动（python grpc_server.py）用于调试。

RPCs:
  ChatStream(ShoppingRequest) → stream ShoppingEvent  — 流式对话
  ResetSession(ResetRequest)  → ResetResponse        — 重置会话
"""
import json, os, sys, time, re, asyncio, grpc

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)
from core.guide_agent import GuideAgent
from core.product_agent import ProductAgent
import shopping_pb2, shopping_pb2_grpc



async def start_grpc_server():
    """启动 gRPC 服务（asyncio 事件循环）"""
    port = int(os.getenv("GRPC_PORT", "50054"))
    server = grpc.aio.server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    shopping_pb2_grpc.add_ShoppingServiceServicer_to_server(
        ShoppingServicer(), server)
    print(f"[grpc] 服务启动: :{port}", flush=True)
    await server.start()
    await server.wait_for_termination()

def _is_guide_done(guide: GuideAgent) -> bool:
    """判断导购阶段是否完成"""
    try:
        status = guide.state_machine.current_state
    except AttributeError:
        return False
    if status is not None:
        v = str(getattr(status, "value", "")).lower()
        n = str(getattr(status, "name", "")).lower()
        if v == "done" or n == "done":
            return True
    return False


class ShoppingServicer(shopping_pb2_grpc.ShoppingServiceServicer):
    """gRPC 服务实现"""

    def __init__(self):
        mcp_port = int(os.getenv("MCP_PORT", "8765"))
        # 构建 LLM + MCP 配置字典（GuideAgent 和 ProductAgent 内部自行创建 LLMClient/MCPClient）
        prompts_dir = os.path.join(ROOT, "prompts")
        def _p(name): return os.path.join(prompts_dir, name)
        self._guide_config = {
            "provider": os.getenv("LLM_PROVIDER", "openai"),
            "base_url": os.getenv("LLM_BASE_URL", "http://localhost:1234/v1"),
            "api_key": os.getenv("LLM_API_KEY", "518678"),
            "model": os.getenv("LLM_MODEL", "google/gemma-4-e4b"),
            "system_prompt": _p("guide_system.txt"),
            "INIT_PROMPT": _p("guide_init.txt"),
            "ASKING_PROMPT": _p("guide_ask.txt"),
            "ASKING_FINAL_PROMPT": _p("guide_ask_intent.txt"),
            "OBSERVING_PROMPT": _p("guide_observe.txt"),
            "OBSERVING_FINAL_PROMPT": _p("guide_observe_intent.txt"),
            "DETAIL_PROMPT": _p("guide_detail.txt"),
            "mcp": {"server_url": f"http://localhost:{mcp_port}/sse", "role": "guide_agent"},
        }
        self._product_config = {
            "provider": os.getenv("LLM_PROVIDER", "openai"),
            "base_url": os.getenv("LLM_BASE_URL", "http://localhost:1234/v1"),
            "api_key": os.getenv("LLM_API_KEY", "518678"),
            "model": os.getenv("LLM_MODEL", "google/gemma-4-e4b"),
            "mcp": {"server_url": f"http://localhost:{mcp_port}/sse", "role": "product_agent"},
        }
        self.guide_agent = GuideAgent(self._guide_config)
        self.product_agent = ProductAgent(self._product_config)
        self._sessions: dict = {}

    async def ChatStream(self, request: shopping_pb2.ShoppingRequest, context):
        msg = request.message
        sid = request.session_id
        uid = request.user_id
        mark = False
        t0 = time.time()

        print(f"[request] ChatStream  sid={sid[:20]} uid={uid[:20]} msg={msg[:40]}...", flush=True)

        # 设置 user_id，确保 load_profile 能加载用户画像
        if uid:
            self.guide_agent.set_user_id(uid, sid)

        yield shopping_pb2.ShoppingEvent(meta=shopping_pb2.MetaEvent(
            session_id=sid, ready=False))

        try:
            if _is_guide_done(self.guide_agent) is False:
                # 导购阶段：GuideAgent 引导用户确认需求
                ok, guide_reply = await self.guide_agent.run(msg)
                result = {"reply": guide_reply or "", "candidates": []}

                # 如果本轮 guide 刚好完成，同一请求内自动触发 ProductAgent
                if _is_guide_done(self.guide_agent):
                    print("[request] Guide just done, auto-switching to ProductAgent", flush=True)
                    product_result = await self.product_agent.run_agent(
                        msg, intent=self.guide_agent.intent)
                    product_reply = product_result.get("reply", "")
                    candidates = product_result.get("candidates", [])
                    # 追加推荐结果到已有回复
                    result["reply"] = (result["reply"] + "\n\n" + product_reply) if product_reply else result["reply"]
                    result["candidates"] = candidates
            else:
                # 产品推荐阶段：从 GuideAgent 传递已确认的 intent 到 ProductAgent
                product_result = await self.product_agent.run_agent(msg, intent=self.guide_agent.intent)
                product_reply = product_result.get("reply", "")
                candidates = product_result.get("candidates", [])
                result = {"reply": product_reply, "candidates": candidates}

        except Exception as e:
            import traceback
            print(f"[agent] ChatStream 失败: {e}", flush=True)
            traceback.print_exc()
            yield shopping_pb2.ShoppingEvent(
                error=shopping_pb2.ErrorEvent(message=str(e)))
            return

        t1 = time.time()
        elapsed_ms = int((t1 - t0) * 1000)

        reply = result.get("reply", "")
        candidates = result.get("candidates", [])

        print(f"[request] ← reply  {len(reply)}B  {elapsed_ms}ms  candidates={len(candidates)}", flush=True)

        # 组装候选产品列表
        done_candidates = []
        for c in candidates[:5]:
            done_candidates.append(shopping_pb2.Candidate(
                name=str(c.get("name", c.get("skuName", ""))),
                price=str(c.get("price", c.get("current_price", "0"))),
                rating=str(c.get("rating", "0")),
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
        sid = request.session_id
        
        # 1. 保存会话数据（如果需要）
        if hasattr(self.guide_agent, 'end_session'):
            self.guide_agent.end_session()

        # 2. 重置 Agent 状态
        try:
            self.guide_agent.state_machine.reset()
        except Exception:
            pass
        try:
            self.product_agent.reset()
        except Exception:
            pass
        
        # 4. 重置会话存储
        if sid in self._sessions:
            del self._sessions[sid]
        
        print(f"[reset] 会话 {sid[:20]} 已重置", flush=True)
        return shopping_pb2.ResetResponse(status="ok")


# ==================== 全流程测试（模拟 ChatStream 的 Guide → Product 转换）====================

# 模拟用户对话脚本：每轮对话会自动判断 Guide 是否完成，完成后自动切换 ProductAgent
CHATSTREAM_TEST_SCRIPT = [
    # Guide 阶段 —— 逐步引导用户确认需求
    "我想买一部手机，拍照要好",
    "3000到5000价位，日常使用",
    "侧重人像拍摄，续航要长，安卓系统",
    "好的就按这个需求推荐吧",
    "确认",
    "是的，确认无误",
]


async def _test_chatstream_flow():
    """模拟 ChatStream 的完整流程：Guide 引导 → 自动转 Product 推荐"""
    servicer = ShoppingServicer()
    guide = servicer.guide_agent
    product = servicer.product_agent

    print("=" * 60)
    print("  ChatStream 全流程测试")
    print("  Guide Agent → Product Agent 自动转换")
    print("=" * 60)

    # ======== Guide 阶段 ========
    for i, msg in enumerate(CHATSTREAM_TEST_SCRIPT, 1):
        if _is_guide_done(guide):
            print(f"\n[Guide] 第{i}轮前已进入 DONE，跳过")
            break

        print(f"\n--- 第{i}轮 (Guide) ---")
        print(f"用户: {msg}")

        ok, reply = await guide.run(msg)
        state = guide.state_machine.current_state
        done = _is_guide_done(guide)

        print(f"状态: {state} | done={done}")
        print(f"回复: {reply[:120] if reply else '(空)'}...")

    # ======== 判断 Guide 是否完成 ========
    print(f"\n{'=' * 60}")
    if not _is_guide_done(guide):
        print("  ❌ Guide 未进入 DONE，无法转换到 ProductAgent")
        print(f"  当前状态: {guide.state_machine.current_state}")
        print(f"  当前 intent: {json.dumps(guide.intent, ensure_ascii=False)[:300]}")
        return
    print("  ✅ Guide 已完成，自动切换到 ProductAgent")
    print(f"  intent: {json.dumps(guide.intent, ensure_ascii=False)}")
    print(f"{'=' * 60}")

    # ======== Product 阶段（与 ChatStream 中完全一致的调用方式）========
    print(f"\n--- ProductAgent 推荐 ---")
    t0 = time.time()
    # 直接用 async 方法避免事件循环嵌套（ChatStream 中 ProductAgent.Run 在非 async 上下文调用，这里在 async 中需直接 await）
    result = await product.run_agent("开始推荐", intent=guide.intent)
    product_reply = result.get("reply", "")
    candidates = result.get("candidates", [])
    elapsed = int((time.time() - t0) * 1000)

    print(f"\n回复 ({elapsed}ms):")
    print(product_reply)
    print(f"\n候选产品 ({len(candidates)}):")
    for i, c in enumerate(candidates, 1):
        print(f"  {i}. {json.dumps(c, ensure_ascii=False, default=str)[:300]}")

    if not candidates:
        print("  ⚠️ 无候选产品返回")

    print(f"\n{'=' * 60}")
    print("  全流程测试完成")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    import threading
    from dotenv import load_dotenv
    load_dotenv()

    # 1. 启动 MCP 服务（后台线程）
    mcp_port = int(os.getenv("MCP_PORT", "8765"))
    from mcp_servers.server import MCPServer
    _mcp = MCPServer(port=mcp_port, host="0.0.0.0")
    _t = threading.Thread(target=_mcp.start, daemon=True, name="mcp-server")
    _t.start()
    time.sleep(2)
    print(f"[main] MCP 服务已启动: :{mcp_port}", flush=True)

    # 2. 运行 ChatStream 全流程测试
    asyncio.run(_test_chatstream_flow())

    # 3. 启动 gRPC 服务对 web 开放
    asyncio.run(start_grpc_server())

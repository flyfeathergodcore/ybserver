"""
购物导购 Agent — 统一入口

同时启动：
  - HTTP 服务（:8080）：用于反向代理 /shopping/ 的 GET 和旧请求
  - gRPC 服务（:50054）：C++ shopping_handler 直连，替代原始 HTTP 调用

两个服务共享相同的 agent 代码和会话管理。
"""
import os, sys, threading, asyncio

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

print("[run] 初始化 agent 模块...", flush=True)
from mcp_servers.mcp_setup import create_mcp_services, create_session_factory
from core.config import load_config

cfg = load_config()
guide_mcp, product_mcp, llm = create_mcp_services(cfg)
sessions, get_or_create_session = create_session_factory(llm, guide_mcp, product_mcp, cfg)


def _run_http():
    """启动 HTTP 服务（同步）"""
    from web_server import start_http_server
    print("[run] HTTP 服务启动...", flush=True)
    start_http_server(get_or_create_session, sessions, llm_client=llm)


def _run_grpc():
    """启动 gRPC 服务（asyncio 事件循环）"""
    from grpc_server import start_grpc_server
    print("[run] gRPC 服务启动...", flush=True)
    asyncio.run(start_grpc_server(get_or_create_session, sessions_dict=sessions, llm_client=llm))


if __name__ == "__main__":
    t = threading.Thread(target=_run_grpc, daemon=True)
    t.start()
    _run_http()

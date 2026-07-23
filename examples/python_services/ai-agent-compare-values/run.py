"""
购物导购 Agent — 统一入口

启动 gRPC 服务（:50054）：C++ shopping_handler 直连。
"""
import os, sys, asyncio
from dotenv import load_dotenv
load_dotenv()  # 加载 .env 文件到 os.environ

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

print("[run] 初始化 agent 模块...", flush=True)
from mcp_servers.server import init_agents
from core.config import load_config

cfg = load_config()
if not init_agents(cfg):
    print("[run] MCP 服务启动失败", flush=True)
    exit(1)


def _run_grpc():
    """启动 gRPC 服务（asyncio 事件循环）"""
    from grpc_server import start_grpc_server
    print("[run] gRPC 服务启动...", flush=True)
    asyncio.run(start_grpc_server())


if __name__ == "__main__":
    _run_grpc()

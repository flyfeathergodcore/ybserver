"""
MCP 服务初始化共享模块。

消除 run.py / grpc_server.py / web_server.py 三处重复的初始化代码。
"""
import os

from mcp_servers.server import build_server
from mcp_servers.client import DirectClient
from core.llm_client import LLMClient
from core.guide_agent import ShoppingGuideAgent
from core.product_agent import ProductAgent
from core.config import load_config


def create_mcp_services(cfg: dict = None, max_workers: int = 4):
    """
    创建 MCP 服务器和客户端。

    返回:
        (guide_mcp: DirectClient, product_mcp: DirectClient, llm: LLMClient)
    """
    if cfg is None:
        cfg = load_config()

    _mcp_server = build_server(cfg, max_workers=max_workers)
    guide_mcp = DirectClient(_mcp_server.registry, _mcp_server.executor, "memory")
    product_mcp = DirectClient(_mcp_server.registry, _mcp_server.executor, "product")

    llm_base = os.getenv("LLM_BASE_URL", cfg.get("llm", {}).get("base_url", "http://ollama:11434/v1"))
    llm_model = os.getenv("LLM_MODEL", cfg.get("llm", {}).get("model", "qwen2.5:0.5b"))
    llm = LLMClient(api_key="ollama", base_url=llm_base, model=llm_model, timeout=120)
    print(f"[mcp_setup] LLM: {llm_model} @ {llm_base}", flush=True)

    return guide_mcp, product_mcp, llm


def create_session_factory(
    llm: LLMClient,
    guide_mcp: DirectClient,
    product_mcp: DirectClient,
    cfg: dict,
):
    """
    创建共享会话存储和 session factory。

    返回:
        (sessions: dict, get_or_create_session: callable)
    """
    sessions: dict[str, dict] = {}

    def get_or_create_session(sid: str) -> dict:
        if sid not in sessions:
            product = ProductAgent(llm=llm, mcp=product_mcp, config=cfg)
            guide = ShoppingGuideAgent(llm=llm, mcp=guide_mcp, config=cfg,
                                       product_agent=product)
            guide.start_session(sid)
            sessions[sid] = {
                "guide": guide,
                "product": product,
                "stage": "guide",
                "candidates": [],
                "last_intent": None,
            }
        return sessions[sid]

    return sessions, get_or_create_session

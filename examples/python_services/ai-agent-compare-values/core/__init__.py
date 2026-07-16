"""
core 模块

- llm_client.py    — LLMClient，OpenAI 兼容 API 客户端
- guide_agent.py   — ShoppingGuideAgent，用户需求分析 + 画像管理 + 短期记忆
- product_agent.py — ProductAgent，产品搜索 + 定位分析 + 生态关系 + 推荐
- config.py        — YAML 配置加载器
"""

from .llm_client import LLMClient
from .guide_agent import ShoppingGuideAgent
from .product_agent import ProductAgent
from .product_data import ProductAnalysis, Recommendation
from .config import load_config

__all__ = [
    "LLMClient",
    "ShoppingGuideAgent",
    "ProductAgent",
    "ProductAnalysis",
    "Recommendation",
    "load_config",
]

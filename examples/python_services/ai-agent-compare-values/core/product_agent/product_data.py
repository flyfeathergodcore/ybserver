"""
产品 Agent 数据结构 — 推荐、产品分析结果。
"""

from dataclasses import dataclass, field


@dataclass
class Recommendation:
    """产品推荐"""
    product: dict
    score: float = 0
    reasons: list[str] = field(default_factory=list)
    caveats: list[str] = field(default_factory=list)
    price_analysis: str = ""


@dataclass
class ProductAnalysis:
    """产品分析结果"""
    query_summary: str = ""
    candidates: list[dict] = field(default_factory=list)
    recommendations: list[Recommendation] = field(default_factory=list)

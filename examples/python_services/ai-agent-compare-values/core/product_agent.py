"""
产品Agent — 商品搜索 + 详情补全 + 定位分析 + 价格对比 + 生态推荐

通过 MCP product 组工具调用京东联盟 API。
启动时拼接所有 skill .md 文档到 system prompt。

定时任务：每 6 小时拉取 jingfen+rank 更新商品库，价格过期自动刷新。
"""

import json
import os
import time
import logging
from dataclasses import dataclass, field
from typing import Optional, Any
from datetime import datetime, timezone, timedelta

from core.llm_client import LLMClient
from core.share_utils import load_text_file

logger = logging.getLogger(__name__)


# ============================================================
# 数据结构
# ============================================================

@dataclass
class PriceAnalysis:
    """价格分析结果"""
    product_id: str
    current_price: float = 0
    lowest_historical: float = 0
    highest_historical: float = 0
    average: float = 0
    price_position: float = 0.5   # 0=最低, 1=最高
    trend: str = "stable"
    verdict: str = ""              # 好价/正常/偏贵/高位
    advice: str = ""
    price_expired: bool = False    # 超过24h未更新
    target_analysis: str = ""


@dataclass
class ProductPosition:
    """产品定位分析"""
    product_id: str
    product_name: str
    tier: str = ""
    target_users: list[str] = field(default_factory=list)
    strengths: list[str] = field(default_factory=list)
    weaknesses: list[str] = field(default_factory=list)
    competitiveness: float = 0.5
    best_for: str = ""


@dataclass
class EcosystemGraph:
    """产品生态图"""
    product_id: str
    accessories: list[dict] = field(default_factory=list)
    cross_category: list[dict] = field(default_factory=list)
    same_brand: list[dict] = field(default_factory=list)
    competitors: list[dict] = field(default_factory=list)
    upgrades: list[dict] = field(default_factory=list)
    complements: list[dict] = field(default_factory=list)


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
    """产品分析完整结果"""
    query_summary: str = ""
    candidates: list[dict] = field(default_factory=list)
    positioning: list[ProductPosition] = field(default_factory=list)
    ecosystems: dict[str, EcosystemGraph] = field(default_factory=dict)
    recommendations: list[Recommendation] = field(default_factory=list)
    price_comparisons: dict[str, PriceAnalysis] = field(default_factory=dict)


# ============================================================
# ProductAgent
# ============================================================

class ProductAgent:
    """
    产品Agent — 搜索→补全→定位→生态→价格对比→推荐。

    构造时自动拼接所有 product skill .md 文档到 system prompt。
    支持定时刷新商品库 + 价格过期检测。
    """

    # 需要拼接的 product skill md 列表
    SKILL_MD_FILES = [
        "jingfen_query.md",
        # "goods_query.md",  # 已屏蔽: goods.query 返回 403
        "ware_search.md",
        "rank_query.md",
        "bigfield_query.md",
        "category_query.md",
        "get_product_detail.md",
        "crawl_price.md",
        "get_price_trend.md",
        "price_compare.md",
        "get_similar_products.md",
        "link_products.md",
        "query_relations.md",
        "find_ecosystem.md",
        "analyze_positioning.md",
    ]

    # 定时刷新参数
    REFRESH_INTERVAL_SECONDS = 6 * 3600    # 每 6 小时刷新
    PRICE_EXPIRY_SECONDS = 24 * 3600        # 价格 24 小时过期

    def __init__(
        self,
        llm: LLMClient,
        mcp: Any = None,
        config: Optional[dict] = None,
        search_temperature: float = 0.2,
        analysis_temperature: float = 0.3,
        default_platform: str = "jd",
        max_candidates: int = 10,
    ):
        self.llm = llm
        self._mcp = mcp

        cfg = config or {}
        product_cfg = cfg.get("product_agent", {})

        # 路径
        self._config_dir = os.path.dirname(os.path.abspath(
            os.getenv("CONFIG_PATH", "config.yaml")
        ))
        self._skills_dir = os.path.join(
            self._config_dir, "mcp_servers", "skills"
        )

        # prompt
        prompt_cfg = cfg.get("prompts", {})
        self._system_prompt_path = prompt_cfg.get(
            "product_system",
            os.path.join(self._config_dir, "prompts", "product_system.txt"),
        )
        self._system_prompt = self._build_system_prompt()

        # 配置
        self._search_temp = search_temperature or product_cfg.get("search_temperature", 0.2)
        self._analysis_temp = analysis_temperature or product_cfg.get("analysis_temperature", 0.3)
        self._default_platform = default_platform or product_cfg.get("default_platform", "jd")
        self._max_candidates = max_candidates or product_cfg.get("max_candidates", 10)

        # 定时刷新
        self._last_refresh: float = 0
        self._refresh_channels = product_cfg.get("refresh_channels", [
            {"type": "jingfen", "elite_id": 24, "limit": 20},
            {"type": "rank", "rank_id": 200006, "sort_type": 2, "limit": 10},
        ])

    # ================================================================
    # System Prompt 构建 — 拼接所有 skill .md 文档
    # ================================================================

    def _build_system_prompt(self) -> str:
        """加载 system prompt 模板，拼接所有 product skill .md 文档。"""
        # 加载基础 prompt
        base = self._load_file(self._system_prompt_path)
        if not base:
            base = "你是商品分析助手，请用中文分析产品定位和推荐理由。"

        # 拼接 skill .md
        skill_docs = []
        for md_file in self.SKILL_MD_FILES:
            path = os.path.join(self._skills_dir, md_file)
            content = self._load_file(path)
            if content:
                skill_docs.append(content)

        if skill_docs:
            skills_text = "\n\n---\n\n".join(skill_docs)
            return f"{base}\n\n# 详细工具文档\n\n{skills_text}"

        return base

    def reload_prompt(self):
        """重新加载 prompt（skill 变更后调用）"""
        self._system_prompt = self._build_system_prompt()
        logger.info("ProductAgent system prompt 已刷新，包含 %d 个 skill 文档",
                     len(self.SKILL_MD_FILES))

    def get_system_prompt(self) -> str:
        return self._system_prompt

    @staticmethod
    def _load_file(path: str) -> str:
        return load_text_file(path, default="", strict=False)

    # ================================================================
    # 核心流程
    # ================================================================

    def search_by_guide_intent(self, intent: dict) -> ProductAnalysis:
        """
        从 guide_agent.analyze_l1() 返回的 intent 快速搜索。
        仅搜索+补全，跳过定位/价格/LLM 推荐（LLM 推荐在 ChatStream 中流式调用）。
        """
        delta = intent.get("intent_delta", {})
        category = intent.get("category", "")
        core_need = delta.get("core_need", "")
        budget = delta.get("budget", {})
        constraints = delta.get("constraints", [])

        search_keyword = category if category else (core_need or "笔记本电脑")
        search_keyword = self._normalize_search_keyword(search_keyword)
        price_min = budget.get("min", 0) or 0
        price_max = budget.get("max", 99999) or 99999
        if price_min > 0 and price_max > 0 and price_max >= price_min and (price_max - price_min) / max(price_max, 1) < 0.3:
            padding = max(price_max * 0.3, 50)
            price_min = max(0, price_min - padding)
            price_max = price_max + padding

        analysis = ProductAnalysis()
        analysis.query_summary = search_keyword
        candidates = self._search_with_fallback(search_keyword, category, price_min, price_max)
        analysis.candidates = candidates
        if candidates:
            analysis.candidates = self._enrich_details(candidates)
        return analysis

    def search_and_analyze(self, info_node: dict) -> ProductAnalysis:
        """
        搜索+补全+定位+价格对比+生态+推荐，完整产品分析流程。

        info_node: {"session_intent": {...}, "user_profile": {...}}
        """
        analysis = ProductAnalysis()

        # 1. 提取搜索参数
        intent = info_node.get("session_intent", {})
        profile = info_node.get("user_profile", {})

        core_need = intent.get("core_need", "")
        budget = intent.get("budget", {})
        constraints = intent.get("constraints", [])
        preferred_categories = profile.get("preferred_categories", [])

        category = preferred_categories[0] if preferred_categories else ""
        # ⭐ 搜索关键词只用品类名，不要拼 core_need 或 constraints
        #   core_need 是对话生成的短语（"想买300左右的键盘"），不适合搜索
        #   constraints 通过 price_min/price_max 过滤，不拼进搜索词
        search_keyword = category if category else (core_need or "笔记本电脑")
        search_keyword = self._normalize_search_keyword(search_keyword)
        price_min = budget.get("min", 0) or 0
        price_max = budget.get("max", 99999) or 99999
        # 放宽价格范围：精确预算（min≈max）展开 ±30%，避免搜不到
        if price_min > 0 and price_max > 0 and price_max >= price_min and (price_max - price_min) / max(price_max, 1) < 0.3:
            padding = max(price_max * 0.3, 50)  # 至少放宽 50 元
            price_min = max(0, price_min - padding)
            price_max = price_max + padding

        analysis.query_summary = search_keyword

        # 2. 搜索商品
        candidates = self._search_with_fallback(search_keyword, category, price_min, price_max)
        analysis.candidates = candidates
        if not candidates:
            return analysis

        # 3. 补全详情（通过 bigfield_query 补参数）
        candidates = self._enrich_details(candidates)

        # 4. 价格对比（每个候选产品）
        target_price = budget.get("current", 0) or 0
        for c in candidates[:self._max_candidates]:
            pid = c.get("id", "")
            price_analysis = self._mcp_price_compare(pid, target_price)
            if price_analysis:
                analysis.price_comparisons[pid] = price_analysis

        # 5. 定位分析 + 生态
        for c in candidates[:self._max_candidates]:
            pid = c.get("id", "")
            pos = self._mcp_analyze_positioning(pid, category)
            if pos:
                analysis.positioning.append(pos)
            eco = self._mcp_find_ecosystem(pid)
            if eco:
                analysis.ecosystems[pid] = eco

        # 6. LLM 生成推荐（含价格分析依据）
        analysis.recommendations = self.generate_recommendations(analysis, info_node)

        return analysis

    # ================================================================
    # 搜索策略：goods_query → jingfen_query → rank_query
    # ================================================================

    @staticmethod
    def _normalize_search_keyword(keyword: str) -> str:
        """修正搜索关键词，避免歧义。

        京东搜索 "笔记本" 会返回纸质笔记本，必须用 "笔记本电脑" 才能搜到电脑。
        """
        mapping = {
            "笔记本": "笔记本电脑",
            "电脑": "笔记本电脑",
            "本": "笔记本电脑",
            "平板": "平板电脑",
        }
        return mapping.get(keyword.strip(), keyword)

    def _search_with_fallback(
        self, query: str, category: str, price_min: float, price_max: float
    ) -> list[dict]:
        """三级搜索降级：ware_search → jingfen → rank"""
        # 1. ware_search（关键词搜索，返回真实商品）
        result = self._mcp_ware_search(query, price_min, price_max, self._max_candidates)
        if result:
            return result

        # 2. jingfen_query（频道浏览 + 客户端过滤）
        result = self._mcp_jingfen_query(query, category, price_min, price_max, self._max_candidates)
        if result:
            return result

        # 3. rank_query（热销排行兜底）
        return self._mcp_rank_query(limit=self._max_candidates)

    def _enrich_details(self, candidates: list[dict]) -> list[dict]:
        """通过 bigfield_query 补全商品参数（类目+属性+图片）。"""
        # 提取 itemId（jingfen 返回的是 itemId 不是 skuId）
        item_ids = []
        for c in candidates:
            iid = c.get("itemId") or c.get("item_id") or ""
            if iid and not c.get("parameters"):  # 已有参数则跳过
                item_ids.append(iid)

        if not item_ids:
            return candidates

        try:
            result = self._mcp_bigfield_query(item_ids=",".join(item_ids[:10]))
            if not result:
                return candidates

            # 合并参数
            enriched_map = {}
            for p in result:
                sku_name = p.get("sku_name", "")
                cat = p.get("category", {})
                params = p.get("parameters", {})
                images = p.get("images", [])
                for c in candidates:
                    if sku_name and sku_name in c.get("name", ""):
                        if cat:
                            c["category"] = cat.get("name", c.get("category", ""))
                        if params:
                            c["parameters"] = params
                        if images and not c.get("image_url"):
                            c["image_url"] = images[0]
                        enriched_map[c.get("id", "")] = True

            logger.info("bigfield 补全了 %d/%d 个产品的参数",
                        len(enriched_map), len(candidates))
        except Exception as e:
            logger.warning("bigfield 补全失败: %s", e)

        return candidates

    # ================================================================
    # 定时刷新商品库
    # ================================================================

    def refresh_product_db(self, force: bool = False) -> dict:
        """
        定时拉取 jingfen + rank 更新商品库和价格记录。

        返回: {"new_products": N, "updated_prices": M, "channels": [...]}
        """
        now = time.time()
        if not force and (now - self._last_refresh) < self.REFRESH_INTERVAL_SECONDS:
            return {"skipped": True, "next_refresh_in": self.REFRESH_INTERVAL_SECONDS - (now - self._last_refresh)}

        stats = {"new_products": 0, "updated_prices": 0, "channels": []}

        for channel in self._refresh_channels:
            ch_type = channel.get("type", "")
            try:
                if ch_type == "jingfen":
                    result = self._mcp_jingfen_query(
                        query="", category="", price_min=0, price_max=99999,
                        limit=channel.get("limit", 20),
                        elite_id=channel.get("elite_id", 24),
                    )
                    stats["channels"].append({
                        "type": "jingfen",
                        "elite_id": channel.get("elite_id", 24),
                        "count": len(result),
                    })
                    stats["new_products"] += len(result)

                elif ch_type == "rank":
                    result = self._mcp_rank_query(
                        rank_id=channel.get("rank_id", 200006),
                        sort_type=channel.get("sort_type", 2),
                        limit=channel.get("limit", 10),
                    )
                    stats["channels"].append({
                        "type": "rank",
                        "rank_id": channel.get("rank_id", 200006),
                        "count": len(result),
                    })
                    stats["new_products"] += len(result)
            except Exception as e:
                logger.error("刷新频道 %s 失败: %s", ch_type, e)

        self._last_refresh = now
        logger.info("商品库刷新完成: %d 新产品", stats["new_products"])
        return stats

    def should_refresh(self) -> bool:
        """检查是否需要刷新"""
        return (time.time() - self._last_refresh) >= self.REFRESH_INTERVAL_SECONDS

    def check_price_expiry(self, product_id: str) -> bool:
        """检查产品价格是否过期（>24h 未更新）"""
        trend = self._mcp_get_price_trend(product_id)
        if not trend:
            return True
        history = trend.get("recent_prices", [])
        if not history:
            return True
        latest_ts = history[0].get("date", 0)
        age = time.time() - latest_ts
        return age > self.PRICE_EXPIRY_SECONDS

    # ================================================================
    # MCP 工具调用封装
    # ================================================================

    def _call_mcp(self, tool_name: str, **kwargs) -> dict:
        """通用 MCP 调用，与 guide_agent.exec_tool 策略一致"""
        if self._mcp is None:
            logger.warning("[_call_mcp] product 组 MCP 不可用")
            return {}
        try:
            raw = self._mcp.call(tool_name, **kwargs)
            result = json.loads(raw) if isinstance(raw, str) else raw
            if isinstance(result, dict) and "error" in result:
                logger.warning("[_call_mcp] %s 调用失败: %s", tool_name, result["error"])
                return {}
            return result
        except Exception as e:
            logger.warning("[_call_mcp] %s 异常: %s", tool_name, e)
            return {}

    def _mcp_ware_search(self, keyword, price_min, price_max, limit):
        r = self._call_mcp("ware_search", keyword=keyword,
                           price_min=price_min, price_max=price_max,
                           sort_type="sort_redissale_desc", limit=limit)
        return r.get("products", [])

    def _mcp_goods_query(self, keyword, price_min, price_max, limit):
        r = self._call_mcp("goods_query", keyword=keyword,
                           price_min=price_min, price_max=price_max,
                           sort_by="sales_volume", limit=limit)
        return r.get("products", [])

    def _mcp_jingfen_query(self, query, category, price_min, price_max, limit, elite_id=0):
        kw = {"query": query, "platform": self._default_platform,
              "price_min": price_min, "price_max": price_max,
              "category": category, "limit": limit}
        if elite_id:
            kw["elite_id"] = elite_id
        r = self._call_mcp("jingfen_query", **kw)
        return r.get("products", [])

    def _mcp_rank_query(self, rank_id=200006, sort_type=2, limit=10):
        r = self._call_mcp("rank_query", rank_id=rank_id,
                           sort_type=sort_type, limit=limit)
        return r.get("products", [])

    def _mcp_bigfield_query(self, item_ids="", sku_ids=""):
        r = self._call_mcp("bigfield_query", item_ids=item_ids, sku_ids=sku_ids)
        return r.get("products", [])

    def _mcp_get_price_trend(self, product_id, days=90):
        return self._call_mcp("get_price_trend", product_id=product_id, days=days)

    def _mcp_price_compare(self, product_id, target_price=0) -> Optional[PriceAnalysis]:
        r = self._call_mcp("price_compare", product_id=product_id,
                           target_price=target_price)
        if not r:
            return None
        analysis = r.get("analysis", {})
        return PriceAnalysis(
            product_id=product_id,
            current_price=analysis.get("current_price", 0),
            lowest_historical=analysis.get("lowest_historical", 0),
            highest_historical=analysis.get("highest_historical", 0),
            average=analysis.get("average", 0),
            price_position=analysis.get("price_position", 0.5),
            trend=analysis.get("trend", "stable"),
            verdict=analysis.get("verdict", ""),
            advice=analysis.get("advice", ""),
            price_expired=r.get("price_expired", False),
            target_analysis=r.get("target_analysis", ""),
        )

    def _mcp_analyze_positioning(self, product_id, category) -> Optional[ProductPosition]:
        r = self._call_mcp("analyze_positioning", product_id=product_id, category=category)
        if not r:
            return None
        pos_data = r.get("positioning", {})
        return ProductPosition(
            product_id=r.get("product_id", product_id),
            product_name=r.get("product_name", ""),
            tier=pos_data.get("tier", ""),
            target_users=pos_data.get("target_users", []),
            strengths=pos_data.get("strengths", []),
            weaknesses=pos_data.get("weaknesses", []),
            competitiveness=pos_data.get("competitiveness", 0.5),
            best_for=pos_data.get("best_for", ""),
        )

    def _mcp_find_ecosystem(self, product_id) -> Optional[EcosystemGraph]:
        r = self._call_mcp("find_ecosystem", product_id=product_id, max_hops=2)
        if not r:
            return None
        graph = r.get("graph_relations", r)
        return EcosystemGraph(
            product_id=product_id,
            accessories=graph.get("accessories", []),
            cross_category=graph.get("cross_category", []),
            same_brand=graph.get("same_brand", []),
            competitors=graph.get("competitors", []),
            upgrades=graph.get("upgrades", []),
            complements=graph.get("complements", []),
        )

    # ================================================================
    # LLM 分析
    # ================================================================

    def generate_recommendations(
        self, analysis: ProductAnalysis, info_node: dict,
        stream_callback=None,  # 可选：逐 token 回调（用于 gRPC 流式推送）
    ) -> list[Recommendation]:
        """
        LLM 生成最终推荐，结合用户画像+产品定位+价格对比。

        stream_callback: 如提供，每收到一个 token 就调用 callback(token)，
                         同时收集完整文本用于 JSON 解析。
        """
        if not analysis.candidates:
            return []

        # 构建上下文：只传 top 3 精简产品给 LLM
        enriched = []
        for c in analysis.candidates[:3]:
            pid = c.get("id", "")
            item = {
                "id": pid,
                "name": c.get("name", ""),
                "current_price": c.get("current_price", 0),
                "rating": c.get("rating", 0),
                "in_stock": c.get("in_stock", True),
                "brand": c.get("brand", ""),
            }
            for pos in analysis.positioning:
                if pos.product_id == pid:
                    item["_positioning"] = {
                        "tier": pos.tier,
                        "best_for": pos.best_for,
                        "competitiveness": pos.competitiveness,
                    }
                    break
            pa = analysis.price_comparisons.get(pid)
            if pa:
                item["_price_analysis"] = {
                    "current": pa.current_price,
                    "lowest": pa.lowest_historical,
                    "verdict": pa.verdict,
                    "advice": pa.advice,
                    "expired": pa.price_expired,
                }
            enriched.append(item)

        context = json.dumps({
            "user_profile": info_node.get("user_profile", {}),
            "core_need": info_node.get("session_intent", {}).get("core_need", ""),
            "budget": info_node.get("session_intent", {}).get("budget", {}),
            "candidates": enriched,
        }, ensure_ascii=False, indent=2)

        prompt = f"""根据用户画像和候选产品（含定位+价格分析）生成推荐。

## 上下文
{context}

## 输出要求
为每个候选产品打分（0~1），考虑需求契合度、预算匹配度、品牌偏好、价格是否合理。

- score: 综合匹配度
- reasons: 为什么推荐，每一条关联用户具体需求
- caveats: 需要注意的问题
- price_analysis: 一句话价格分析（引用_PriceAnalysis中的数据）

只输出 JSON:
{{"recommendations":[{{"product":{{原始产品}},"score":0.85,"reasons":[],"caveats":[],"price_analysis":""}}]}}"""

        _rec_system = ("你是京东商品推荐助手。根据用户需求和候选产品（已附定位分析和价格走势），"
                       "为每个产品打分并给出推荐理由。只输出 JSON。")
        try:
            if stream_callback is not None:
                # ── 流式模式：逐 token 回调，同时累积全文 ──
                full_text = ""
                for token in self.llm.chat_stream(prompt, system_prompt=_rec_system):
                    full_text += token
                    stream_callback(token)
                # 尝试解析 JSON
                import re
                m = re.search(r'\{[\s\S]*\}', full_text)
                if m:
                    result = json.loads(m.group())
                else:
                    result = {}
                items = result if isinstance(result, list) else result.get("recommendations", [])
            else:
                # ── 非流式模式（原有逻辑，加大超时） ──
                old_timeout = self.llm.timeout
                self.llm.timeout = max(self.llm.timeout, 300)  # 至少 5 分钟
                try:
                    result = self.llm.chat_with_json(
                        prompt, system_prompt=_rec_system, temperature=self._search_temp
                    )
                finally:
                    self.llm.timeout = old_timeout
                items = result if isinstance(result, list) else result.get("recommendations", [])

            recommendations = []
            for item in items:
                recommendations.append(Recommendation(
                    product=item.get("product", {}),
                    score=item.get("score", 0),
                    reasons=item.get("reasons", []),
                    caveats=item.get("caveats", []),
                    price_analysis=item.get("price_analysis", ""),
                ))
            return recommendations
        except Exception as e:
            logger.error("LLM 推荐生成失败: %s", e)
            return []

    def answer_product_question(
        self,
        question: str,
        candidates: list[dict] | None = None,
    ) -> str:
        """
        回答用户对某款产品的追问（"这款值得买吗？""和XX比哪个好？"）。

        流程:
        1. 检查产品是否存在（候选列表为空 → 不存在；匹配不到 → 不在列表中）
        2. 调 MCP 获取详情+价格+定位+生态
        3. LLM 综合回答
        """
        candidates = candidates or []

        # 1. 检查产品是否存在
        if not candidates:
            return "抱歉，当前没有搜索到相关产品，请换个关键词重新搜索。🔄"

        matched_id, matched_name = self._match_product_from_question(question, candidates)

        if not matched_id:
            return f"你问的产品不在当前的推荐列表中。以下是推荐的产品：\n" + "\n".join(
                f"- {c.get('sname', c.get('name', '?'))[:30]}" for c in candidates[:5]
            ) + "\n\n请从以上产品中选择，或者复制产品名称告诉我。"

        # 2. 调 MCP 收集数据
        detail = self._call_mcp("get_product_detail", product_id=matched_id)
        price = self._call_mcp("price_compare", product_id=matched_id)
        positioning = self._call_mcp("analyze_positioning", product_id=matched_id)
        ecosystem = self._call_mcp("find_ecosystem", product_id=matched_id, max_hops=1)

        # 3. 构建上下文
        context_parts = [f"## 产品\n名称: {matched_name} (ID: {matched_id})"]

        product_data = detail.get("product", detail)
        if isinstance(product_data, dict):
            params = product_data.get("parameters", {})
            if params:
                context_parts.append(f"\n### 参数\n{json.dumps(params, ensure_ascii=False, indent=2)}")

        if price:
            pa = price.get("analysis", price)
            context_parts.append(f"\n### 价格分析\n{json.dumps(pa, ensure_ascii=False, indent=2)}")

        if positioning:
            pos = positioning.get("positioning", positioning)
            context_parts.append(f"\n### 市场定位\n{json.dumps(pos, ensure_ascii=False, indent=2)}")

        if ecosystem:
            eco = ecosystem.get("graph_relations", ecosystem)
            context_parts.append(f"\n### 生态（竞品/配件）\n{json.dumps(eco, ensure_ascii=False, indent=2)}")

        context = "\n".join(context_parts)

        prompt = f"""用户对推荐产品有疑问，请根据以下数据回答。

{context}

## 用户问题
{question}

## 输出要求
- 用中文、口语化回答
- 引用具体数据（价格、参数、定位）
- 给出明确建议（值得买 / 可以考虑 / 不推荐）
- 如果数据不足，如实告诉用户
"""
        try:
            reply = self.llm.chat(prompt, system_prompt=self._system_prompt, temperature=0.3)
            return reply.strip()
        except Exception as e:
            return f"分析失败: {e}"

    def _match_product_from_question(
        self, question: str, candidates: list[dict]
    ) -> tuple[str, str]:
        """从用户问题中匹配候选产品，返回 (id, name)"""
        if not candidates:
            return ("", "")

        # 按问题中包含的产品名匹配
        q_lower = question.lower()
        for c in candidates:
            name = c.get("name", c.get("skuName", ""))
            sname = c.get("sname", "")
            if name and (name[:8].lower() in q_lower or q_lower[:8] in name[:8].lower()):
                return (c.get("id", ""), name)
            if sname and (sname.lower() in q_lower or q_lower[:8] in sname.lower()):
                return (c.get("id", ""), name)

        # 按"第一个""这款"等模糊匹配
        import re
        if re.search(r'[第一二三四五六七八九十]个|这款|第一个|首选|推荐.*[12]', q_lower):
            return (candidates[0].get("id", ""), candidates[0].get("name", ""))

        return ("", "")

    def compare_products(self, product_ids: list[str]) -> dict:
        """多产品横向对比（参数+价格+生态）。"""
        products = []
        for pid in product_ids:
            r = self._call_mcp("get_product_detail", product_id=pid)
            product = r.get("product", {})
            if product:
                products.append(product)

        if len(products) < 2:
            return {"error": "至少需要2个产品", "products": products}

        # 关键参数对比
        key_params = ["CPU", "GPU", "RAM", "存储", "屏幕", "重量", "续航"]
        comparison = {"products": products, "comparison": {}}

        for key in key_params:
            row = {}
            for p in products:
                params = p.get("parameters", {})
                row[p.get("name", "?")] = params.get(key, "N/A")
            comparison["comparison"][key] = row

        # 价格对比
        comparison["comparison"]["价格"] = {
            p.get("name", "?"): f"¥{p.get('current_price', 'N/A')}"
            for p in products
        }
        comparison["comparison"]["评分"] = {
            p.get("name", "?"): p.get("rating", "N/A")
            for p in products
        }

        return comparison

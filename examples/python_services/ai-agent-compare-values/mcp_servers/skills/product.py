import sys, os
_project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, _project_root)

import logging
from mcp.server.fastmcp import FastMCP
from typing import Optional
import json

logger = logging.getLogger(__name__)
logger.info("模块加载完成，项目根目录: %s", _project_root)

# ── GraphRAG Neo4j 集成 ──
_graphrag_tool_dir = os.path.join(_project_root, 'graphrag', 'tool')
if _graphrag_tool_dir not in sys.path:
    sys.path.insert(0, _graphrag_tool_dir)

_graphrag_pool = None       # Neo4jConnectionPool 实例
_graphrag_queries = None    # GraphRAGQueries 实例


def _get_graphrag():
    """延迟初始化 Neo4j 连接池和 GraphRAG 查询对象（单例）"""
    global _graphrag_pool, _graphrag_queries
    if _graphrag_queries is not None:
        # 健康检查：如果连接池已关闭，则重新初始化
        if _graphrag_pool is not None and _graphrag_pool._is_closed:
            _graphrag_pool = None
            _graphrag_queries = None
        else:
            return _graphrag_queries

    import dbdriver as _gdb
    import Graphrag as _gq

    neo4j_uri = os.getenv("NEO4J_URI", "bolt://localhost:7687")
    neo4j_user = os.getenv("NEO4J_USER", "neo4j")
    neo4j_password = os.getenv("NEO4J_PASSWORD", "neo4j123456")

    logger.info("初始化 GraphRAG 连接池: %s", neo4j_uri)
    _graphrag_pool = _gdb.Neo4jConnectionPool(
        uri=neo4j_uri,
        user=neo4j_user,
        password=neo4j_password,
        max_pool_size=5,
        min_pool_size=1,
    )
    _graphrag_queries = _gq.GraphRAGQueries(_graphrag_pool)
    logger.info("GraphRAG 初始化完成，连接池状态: %s", _graphrag_pool.get_pool_status())
    return _graphrag_queries


def register_skill(fastmcp: FastMCP) -> None:
    """注册产品agent的skill到FastMCP框架中。"""

    # ==================== GraphRAG Neo4j 知识图谱工具 ====================

    @fastmcp.tool()
    def search_graphrag(keyword: str, limit: int = 10) -> list[dict]:
        """
        在 Neo4j 知识图谱中搜索商品实体（手机/笔记本/品牌等），支持模糊匹配。

        基于 GraphRAG 全文索引和模糊搜索，可从名称、品牌、系列等维度查找。

        Args:
            keyword: 搜索关键词，如"iPhone"、"华为"、"MacBook"、"游戏本"
            limit: 返回条数上限，默认 10

        Returns:
            实体列表，每项包含实体属性（name, brand, jd_price 等）
        """
        logger.info("GraphRAG 搜索: keyword=%s limit=%d", keyword, limit)
        try:
            gq = _get_graphrag()
            results = gq.search_entities(keyword, limit=limit)
            # 将 Neo4j Node 对象序列化为 dict
            serialized = []
            for r in results:
                e = r.get('e', {})
                if hasattr(e, '_properties'):
                    serialized.append(dict(e._properties))
                elif isinstance(e, dict):
                    serialized.append(e)
                else:
                    serialized.append({"data": str(e)})
            logger.info("GraphRAG 搜索完成: keyword=%s 返回 %d 条", keyword, len(serialized))
            return serialized
        except Exception as e:
            logger.error("GraphRAG 搜索失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def get_phone_recommendations(phone_id: str, limit: int = 10,
                                  min_similarity: float = 0.3) -> list[dict]:
        """
        根据手机 ID 获取智能推荐（基于属性相似度 + 价格相近度评分）。

        从 Neo4j 知识图谱中查找与目标手机相似的机型，按相似度评分降序排列。

        Args:
            phone_id: 手机的唯一标识（zol_id 或 phone_id），可通过 search_graphrag 获取
            limit: 返回推荐数量上限，默认 10
            min_similarity: 最小相似度阈值（0~10），默认 0.3

        Returns:
            推荐手机列表，每项包含：
              - related_phone: 推荐手机属性
              - relation_label: 关系标签（同品牌/同价位/同尺寸/相似）
              - similarity_score: 相似度评分（越高越相似）
        """
        logger.info("手机推荐: phone_id=%s limit=%d min_sim=%.1f", phone_id, limit, min_similarity)
        try:
            gq = _get_graphrag()
            results = gq.get_phone_relationships(phone_id, limit=limit, min_similarity=min_similarity)
            serialized = []
            for r in results:
                item = {
                    "relation_label": r.get("relation_label", ""),
                    "similarity_score": r.get("similarity_score", 0),
                }
                related = r.get("related_phone", {})
                if hasattr(related, '_properties'):
                    item["related_phone"] = dict(related._properties)
                elif isinstance(related, dict):
                    item["related_phone"] = related
                serialized.append(item)
            logger.info("手机推荐完成: phone_id=%s 返回 %d 条", phone_id, len(serialized))
            return serialized
        except Exception as e:
            logger.error("手机推荐查询失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def compare_phones_graph(phone_id_1: str, phone_id_2: str) -> dict:
        """
        在 Neo4j 知识图谱中对比两款手机的详细参数。

        返回品牌、系统、屏幕、价格、5G、NFC 等多维度相似度，以及价格差、配置对比。

        Args:
            phone_id_1: 第一款手机 ID（如 "2139583"）
            phone_id_2: 第二款手机 ID（如 "2139584"）

        Returns:
            对比结果 dict，包含：
              - similarity: 各维度相似度及总分（0~1）
              - comparison: 具体配置对比（价格差、品牌、系统、屏幕、电池等）
        """
        logger.info("手机对比: %s vs %s", phone_id_1, phone_id_2)
        try:
            gq = _get_graphrag()
            result = gq.get_phone_comparison(phone_id_1, phone_id_2)
            logger.info("手机对比完成: %s vs %s", phone_id_1, phone_id_2)
            return result if result else {"error": "未找到对比结果"}
        except Exception as e:
            logger.error("手机对比失败: %s", e)
            return {"error": str(e)}

    @fastmcp.tool()
    def search_phones_by_price(min_price: int, max_price: int) -> list[dict]:
        """
        按京东价格区间查询 Neo4j 知识图谱中的手机。

        Args:
            min_price: 最低价格（单位：元），如 3000
            max_price: 最高价格（单位：元），如 6000

        Returns:
            手机列表，按价格升序排列，每项包含完整手机属性
        """
        logger.info("价格区间查询: %d~%d 元", min_price, max_price)
        try:
            gq = _get_graphrag()
            results = gq.get_phones_by_price_range(min_price, max_price)
            serialized = []
            for r in results:
                p = r.get('p', {})
                if hasattr(p, '_properties'):
                    serialized.append(dict(p._properties))
                elif isinstance(p, dict):
                    serialized.append(p)
            logger.info("价格区间查询完成: %d~%d 返回 %d 条", min_price, max_price, len(serialized))
            return serialized
        except Exception as e:
            logger.error("价格区间查询失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def get_same_brand_phones(brand: str, limit: int = 10) -> list[dict]:
        """
        按品牌查询 Neo4j 知识图谱中的同品牌手机，按价格升序排列。

        Args:
            brand: 品牌名称，如"苹果"、"华为"、"小米"、"三星"
            limit: 返回数量上限，默认 10

        Returns:
            手机列表，每项包含完整手机属性及 price 字段（数值）
        """
        logger.info("同品牌查询: brand=%s limit=%d", brand, limit)
        try:
            gq = _get_graphrag()
            results = gq.get_similar_phones_by_brand(brand, limit=limit)
            serialized = []
            for r in results:
                p = r.get('p', {})
                price = r.get('price', 0)
                if hasattr(p, '_properties'):
                    item = dict(p._properties)
                elif isinstance(p, dict):
                    item = dict(p)
                else:
                    item = {}
                item['price'] = price
                serialized.append(item)
            logger.info("同品牌查询完成: brand=%s 返回 %d 条", brand, len(serialized))
            return serialized
        except Exception as e:
            logger.error("同品牌查询失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def get_competitive_phones_graph(phone_id: str, limit: int = 5) -> list[dict]:
        """
        查询 Neo4j 知识图谱中指定手机的竞争机型（同价位、同尺寸、不同品牌）。

        用于分析市场竞争格局，标注竞争强度（强竞争/中等竞争/弱竞争）。

        Args:
            phone_id: 目标手机 ID
            limit: 返回数量上限，默认 5

        Returns:
            竞争机型列表，每项包含：
              - related_phone: 竞争手机属性
              - competitive_level: 竞争强度（强竞争/中等竞争/弱竞争）
              - similarity_score: 相似度评分
        """
        logger.info("竞争机型查询: phone_id=%s limit=%d", phone_id, limit)
        try:
            gq = _get_graphrag()
            results = gq.get_competitive_phones(phone_id, limit=limit)
            serialized = []
            for r in results:
                item = {
                    "relation_label": r.get("relation_label", ""),
                    "competitive_level": r.get("competitive_level", ""),
                    "similarity_score": r.get("similarity_score", 0),
                }
                related = r.get("related_phone", {})
                if hasattr(related, '_properties'):
                    item["related_phone"] = dict(related._properties)
                elif isinstance(related, dict):
                    item["related_phone"] = related
                serialized.append(item)
            logger.info("竞争机型查询完成: phone_id=%s 返回 %d 条", phone_id, len(serialized))
            return serialized
        except Exception as e:
            logger.error("竞争机型查询失败: %s", e)
            return [{"error": str(e)}]

    # ==================== 笔记本 GraphRAG 工具 ====================

    @fastmcp.tool()
    def search_laptops(keyword: str, limit: int = 10) -> list[dict]:
        """
        在 Neo4j 知识图谱中搜索笔记本（按名称/品牌模糊匹配）。

        Args:
            keyword: 搜索关键词，如"华为"、"ThinkPad"、"游戏本"
            limit: 返回条数上限，默认 10

        Returns:
            笔记本列表，每项包含 name、brand、positioning、screen_class 等属性
        """
        logger.info("笔记本搜索: keyword=%s limit=%d", keyword, limit)
        try:
            gq = _get_graphrag()
            results = gq.search_laptops(keyword, limit=limit)
            serialized = []
            for r in results:
                e = r.get('e', {})
                if hasattr(e, '_properties'):
                    serialized.append(dict(e._properties))
                elif isinstance(e, dict):
                    serialized.append(e)
            logger.info("笔记本搜索完成: keyword=%s 返回 %d 条", keyword, len(serialized))
            return serialized
        except Exception as e:
            logger.error("笔记本搜索失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def get_laptop_recommendations(zol_id: str, limit: int = 10) -> list[dict]:
        """
        根据笔记本 zol_id 获取相似笔记本推荐（基于定位、屏幕尺寸、重量）。

        Args:
            zol_id: 笔记本的 zol_id，可通过 search_laptops 获取
            limit: 返回数量上限，默认 10

        Returns:
            推荐笔记本列表，每项包含 laptop、brand、similarity_score、relation_label
        """
        logger.info("笔记本推荐: zol_id=%s limit=%d", zol_id, limit)
        try:
            gq = _get_graphrag()
            results = gq.get_laptop_recommendations(zol_id, limit=limit)
            serialized = []
            for r in results:
                item = {"relation_label": r.get("relation_label", ""),
                        "similarity_score": r.get("similarity_score", 0),
                        "brand": r.get("brand", "")}
                laptop = r.get("laptop", {})
                if hasattr(laptop, '_properties'):
                    item["laptop"] = dict(laptop._properties)
                elif isinstance(laptop, dict):
                    item["laptop"] = laptop
                serialized.append(item)
            logger.info("笔记本推荐完成: zol_id=%s 返回 %d 条", zol_id, len(serialized))
            return serialized
        except Exception as e:
            logger.error("笔记本推荐查询失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def compare_laptops(zol_id_1: str, zol_id_2: str) -> dict:
        """
        对比两款笔记本的详细参数（定位、重量、屏幕、品牌、CPU、GPU）。

        Args:
            zol_id_1: 第一款笔记本 zol_id
            zol_id_2: 第二款笔记本 zol_id

        Returns:
            对比结果，包含 laptop1/laptop2 属性、brand/cpu/gpu 对比、similarity 评分
        """
        logger.info("笔记本对比: %s vs %s", zol_id_1, zol_id_2)
        try:
            gq = _get_graphrag()
            result = gq.compare_laptops(zol_id_1, zol_id_2)
            return result if result else {"error": "未找到对比结果"}
        except Exception as e:
            logger.error("笔记本对比失败: %s", e)
            return {"error": str(e)}

    @fastmcp.tool()
    def get_same_brand_laptops(brand: str, limit: int = 10) -> list[dict]:
        """
        按品牌查询同品牌笔记本。

        Args:
            brand: 品牌名称，如"华为"、"联想"、"苹果"、"戴尔"
            limit: 返回数量上限，默认 10

        Returns:
            笔记本列表，每项包含 laptop 属性和 brand 名称
        """
        logger.info("同品牌笔记本: brand=%s limit=%d", brand, limit)
        try:
            gq = _get_graphrag()
            results = gq.get_same_brand_laptops(brand, limit=limit)
            serialized = []
            for r in results:
                item = {"brand": r.get("brand", "")}
                l = r.get("l", {})
                if hasattr(l, '_properties'):
                    item["laptop"] = dict(l._properties)
                elif isinstance(l, dict):
                    item["laptop"] = l
                serialized.append(item)
            logger.info("同品牌笔记本完成: brand=%s 返回 %d 条", brand, len(serialized))
            return serialized
        except Exception as e:
            logger.error("同品牌笔记本查询失败: %s", e)
            return [{"error": str(e)}]

    @fastmcp.tool()
    def get_laptops_by_positioning(positioning: str, limit: int = 10) -> list[dict]:
        """
        按产品定位查找笔记本（如：游戏本、轻薄本、商务本、创意设计本）。

        Args:
            positioning: 产品定位关键词，如"游戏"、"轻薄"、"商务"、"设计"
            limit: 返回数量上限，默认 10

        Returns:
            笔记本列表
        """
        logger.info("按定位查笔记本: positioning=%s limit=%d", positioning, limit)
        try:
            gq = _get_graphrag()
            results = gq.get_laptops_by_positioning(positioning, limit=limit)
            serialized = []
            for r in results:
                item = {"brand": r.get("brand", "")}
                l = r.get("l", {})
                if hasattr(l, '_properties'):
                    item["laptop"] = dict(l._properties)
                elif isinstance(l, dict):
                    item["laptop"] = l
                serialized.append(item)
            logger.info("按定位查笔记本完成: %s 返回 %d 条", positioning, len(serialized))
            return serialized
        except Exception as e:
            logger.error("按定位查笔记本失败: %s", e)
            return [{"error": str(e)}]

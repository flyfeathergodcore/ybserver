from typing import List, Dict, Optional
import dbdriver as dbdriver
import logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class GraphRAGQueries:
    """GraphRAG 相关的查询"""
    
    def __init__(self, pool: dbdriver.Neo4jConnectionPool):
        self.pool = pool
    
    def get_entity_by_id(self, entity_id: str) -> Optional[Dict]:
        """根据ID获取实体"""
        query = """
        MATCH (e {id: $id})
        RETURN e
        """
        result = self.pool.execute_query(query, {"id": entity_id})
        return result[0] if result else None
    
    def get_entity_relations(self, entity_id: str, depth: int = 2) -> List[Dict]:
        """获取实体关联"""
        query = f"""
        MATCH path = (e {{id: $id}})-[*1..{depth}]-(related)
        RETURN path, nodes(path) as nodes, relationships(path) as rels
        """
        return self.pool.execute_query(query, {"id": entity_id})
    
    def search_entities(self, keyword: str, limit: int = 10) -> List[Dict]:
        """搜索实体（优先全文索引，回退到 CONTAINS 模糊匹配）"""
        # 尝试使用全文索引
        query_ft = """
        CALL db.index.fulltext.queryNodes('entity_index', $keyword)
        YIELD node, score
        RETURN node as e, score
        ORDER BY score DESC
        LIMIT $limit
        """
        try:
            results = self.pool.execute_query(
                query_ft, {"keyword": keyword, "limit": limit},
                retry=1, count_as_error=False
            )
            if results:
                return results
        except:
            pass

        # 全文索引不存在或返回空 → 使用普通 CONTAINS 搜索
        logger.info("📝 使用普通搜索")
        query_like = """
        MATCH (e)
        WHERE toLower(e.name) CONTAINS toLower($keyword)
           OR toLower(e.brand) CONTAINS toLower($keyword)
           OR toLower(e.series) CONTAINS toLower($keyword)
        RETURN e
        LIMIT $limit
        """
        return self.pool.execute_query(
            query_like, {"keyword": keyword, "limit": limit}, count_as_error=False
        )
    
    def get_phone_by_attributes(self, attributes: Dict) -> List[Dict]:
        """根据属性查询手机"""
        conditions = []
        params = {}
        
        for key, value in attributes.items():
            if value:
                conditions.append(f"p.{key} = ${key}")
                params[key] = value
        
        if not conditions:
            return []
        
        where_clause = " AND ".join(conditions)
        query = f"""
        MATCH (p:Phone)
        WHERE {where_clause}
        RETURN p
        """
        return self.pool.execute_query(query, params, count_as_error=False)
    
    def get_phones_by_price_range(self, min_price: int, max_price: int) -> List[Dict]:
        """根据价格范围查询手机"""
        query = """
        MATCH (p:Phone)
        WHERE p.jd_price IS NOT NULL
          AND toInteger(replace(p.jd_price, '¥', '')) >= $min_price
          AND toInteger(replace(p.jd_price, '¥', '')) <= $max_price
        RETURN p
        ORDER BY toInteger(replace(p.jd_price, '¥', '')) ASC
        """
        return self.pool.execute_query(query, {"min_price": min_price, "max_price": max_price}, count_as_error=False)
    
    def get_phone_relationships(self, phone_id: str, limit: int = 10, min_similarity: float = 0.3) -> List[Dict]:
        """
        获取手机的关联关系（智能推荐）
        
        Args:
            phone_id: 手机ID
            limit: 返回结果数量
            min_similarity: 最小相似度阈值
        """
        
        # 方案1: 基于属性相似度推荐
        query_similar = """
        MATCH (p:Phone {phone_id: $phone_id})
        MATCH (other:Phone)
        WHERE other.phone_id <> $phone_id
        AND other.brand IS NOT NULL
        WITH p, other,
            CASE WHEN p.brand = other.brand THEN 3 ELSE 0 END +
            CASE WHEN p.os = other.os THEN 2 ELSE 0 END +
            CASE WHEN p.screen_class = other.screen_class THEN 1 ELSE 0 END +
            CASE WHEN p.price_class = other.price_class THEN 2 ELSE 0 END +
            CASE WHEN p.has_5g = other.has_5g THEN 1 ELSE 0 END +
            CASE WHEN p.nfc = other.nfc THEN 1 ELSE 0 END +
            CASE WHEN p.waterproof = other.waterproof THEN 1 ELSE 0 END +
            CASE 
                WHEN p.jd_price IS NOT NULL AND other.jd_price IS NOT NULL THEN
                    10 - abs(toInteger(replace(p.jd_price, '¥', '')) - 
                            toInteger(replace(other.jd_price, '¥', ''))) / 1000
                ELSE 0
            END as similarity_score
        WHERE similarity_score >= $min_similarity
        OPTIONAL MATCH (p)-[r:COMPARE_WITH]-(other)
        RETURN other as related_phone, 
               r as relationship,
               similarity_score,
               CASE 
                   WHEN p.brand = other.brand AND p.os = other.os THEN '同品牌同系统'
                   WHEN p.brand = other.brand THEN '同品牌'
                   WHEN p.price_class = other.price_class THEN '同价位'
                   WHEN p.screen_class = other.screen_class THEN '同尺寸'
                   ELSE '相似'
               END as relation_label
        ORDER BY similarity_score DESC, other.name
        LIMIT $limit
        """
        
        try:
            results = self.pool.execute_query(
                query_similar, 
                {
                    "phone_id": phone_id, 
                    "limit": limit,
                    "min_similarity": min_similarity
                },
                count_as_error=False
            )
            if results:
                return results
        except Exception as e:
            logger.warning(f"相似度查询失败: {e}")
        
        # 方案2: 基于品牌和价格推荐（简单版本）
        query_simple = """
        MATCH (p:Phone {phone_id: $phone_id})
        MATCH (other:Phone)
        WHERE other.phone_id <> $phone_id
        AND (other.brand = p.brand 
            OR other.price_class = p.price_class
            OR other.screen_class = p.screen_class)
        AND NOT EXISTS((p)-[:COMPARE_WITH]-(other))
        RETURN other as related_phone,
               null as relationship,
               CASE 
                   WHEN other.brand = p.brand AND other.price_class = p.price_class 
                       THEN '同品牌同价位'
                   WHEN other.brand = p.brand THEN '同品牌'
                   WHEN other.price_class = p.price_class THEN '同价位'
                   ELSE '相似配置'
               END as relation_label,
               5 as similarity_score
        ORDER BY other.name
        LIMIT $limit
        """
        
        return self.pool.execute_query(
            query_simple,
            {"phone_id": phone_id, "limit": limit},
            count_as_error=False
        )
    
    def get_phone_comparison(self, phone_id_1: str, phone_id_2: str) -> Dict:
        """获取两个手机的详细对比信息"""
        query = """
        MATCH (p1:Phone {phone_id: $phone_id_1})
        MATCH (p2:Phone {phone_id: $phone_id_2})
        
        WITH p1, p2,
             CASE WHEN p1.brand = p2.brand THEN 1 ELSE 0 END as same_brand,
             CASE WHEN p1.os = p2.os THEN 1 ELSE 0 END as same_os,
             CASE WHEN p1.screen_class = p2.screen_class THEN 1 ELSE 0 END as same_screen,
             CASE WHEN p1.price_class = p2.price_class THEN 1 ELSE 0 END as same_price,
             CASE WHEN p1.has_5g = p2.has_5g THEN 1 ELSE 0 END as same_5g,
             CASE WHEN p1.nfc = p2.nfc THEN 1 ELSE 0 END as same_nfc
        
        OPTIONAL MATCH (p1)-[r:COMPARE_WITH]-(p2)
        
        RETURN {
            phone1: p1,
            phone2: p2,
            similarity: {
                brand: same_brand,
                os: same_os,
                screen: same_screen,
                price: same_price,
                has_5g: same_5g,
                nfc: same_nfc,
                total: (same_brand + same_os + same_screen + same_price + same_5g + same_nfc) / 6.0
            },
            has_relationship: r IS NOT NULL,
            relationship: r,
            comparison: {
                price_diff: CASE 
                    WHEN p1.jd_price IS NOT NULL AND p2.jd_price IS NOT NULL THEN
                        abs(toInteger(replace(p1.jd_price, '¥', '')) - 
                            toInteger(replace(p2.jd_price, '¥', '')))
                    ELSE null
                END,
                brand: [p1.brand, p2.brand],
                os: [p1.os, p2.os],
                screen_size: [p1.screen_size, p2.screen_size],
                battery: [p1.battery_class, p2.battery_class],
                camera: [p1.camera_pixels, p2.camera_pixels],
                weight: [p1.weight, p2.weight],
                storage: [p1.storage, p2.storage],
                ram: [p1.ram, p2.ram]
            }
        } as result
        """
        
        results = self.pool.execute_query(
            query,
            {"phone_id_1": phone_id_1, "phone_id_2": phone_id_2},
            count_as_error=False
        )
        
        return results[0]['result'] if results else {}
    
    def get_similar_phones_by_brand(self, brand: str, limit: int = 10) -> List[Dict]:
        """获取同品牌手机推荐"""
        query = """
        MATCH (p:Phone)
        WHERE p.brand = $brand
          AND p.name IS NOT NULL
        RETURN p,
               toInteger(replace(p.jd_price, '¥', '')) as price
        ORDER BY price ASC
        LIMIT $limit
        """
        return self.pool.execute_query(
            query,
            {"brand": brand, "limit": limit},
            count_as_error=False
        )
    
    def get_competitive_phones(self, phone_id: str, limit: int = 5) -> List[Dict]:
        """获取竞争机型（相似价位、相似配置的其他品牌）"""
        query = """
        MATCH (p:Phone {phone_id: $phone_id})
        MATCH (other:Phone)
        WHERE other.phone_id <> $phone_id
          AND other.brand <> p.brand
          AND other.price_class = p.price_class
          AND other.screen_class = p.screen_class
        
        WITH p, other,
             CASE WHEN other.os = p.os THEN 2 ELSE 0 END +
             CASE WHEN other.has_5g = p.has_5g THEN 1 ELSE 0 END +
             CASE WHEN other.nfc = p.nfc THEN 1 ELSE 0 END +
             CASE WHEN other.waterproof = p.waterproof THEN 1 ELSE 0 END as similarity_score
        
        OPTIONAL MATCH (p)-[r:COMPARE_WITH]-(other)
        
        RETURN other as related_phone,
               r as relationship,
               '竞争机型' as relation_label,
               similarity_score,
               CASE 
                   WHEN similarity_score >= 3 THEN '强竞争'
                   WHEN similarity_score >= 2 THEN '中等竞争'
                   ELSE '弱竞争'
               END as competitive_level
        ORDER BY similarity_score DESC, other.name
        LIMIT $limit
        """
        
        return self.pool.execute_query(
            query,
            {"phone_id": phone_id, "limit": limit},
            count_as_error=False
        )


    # ==================== 笔记本查询 ====================

    def search_laptops(self, keyword: str, limit: int = 10) -> List[Dict]:
        """搜索笔记本（按名称/品牌）"""
        results = self.search_entities(keyword, limit=limit)
        # 只返回 Laptop 标签的节点
        laptops = []
        for r in results:
            e = r.get('e', {})
            labels = getattr(e, 'labels', None) or e.get('labels', [])
            if 'Laptop' in labels:
                laptops.append(r)
        return laptops[:limit]

    def get_laptop_recommendations(self, zol_id: str, limit: int = 10) -> List[Dict]:
        """获取相似笔记本推荐（基于定位、屏幕尺寸、重量）"""
        query = """
        MATCH (l:Laptop {zol_id: $zol_id})
        MATCH (other:Laptop)
        WHERE other.zol_id <> $zol_id
        WITH l, other,
            CASE WHEN l.positioning = other.positioning AND l.positioning <> '未分类' THEN 3 ELSE 0 END +
            CASE WHEN l.screen_class = other.screen_class AND l.screen_class <> '未知' THEN 2 ELSE 0 END +
            CASE WHEN l.weight_class = other.weight_class AND l.weight_class <> '未知' THEN 2 ELSE 0 END +
            CASE WHEN l.port_class = other.port_class AND l.port_class <> '无接口' THEN 1 ELSE 0 END as similarity_score
        MATCH (other)-[:BELONGS_TO]->(b:Brand)
        RETURN other as laptop,
               b.name as brand,
               similarity_score,
               CASE
                   WHEN l.positioning = other.positioning AND l.positioning <> '未分类' THEN '同定位'
                   WHEN l.screen_class = other.screen_class AND l.screen_class <> '未知' THEN '同尺寸'
                   WHEN l.weight_class = other.weight_class AND l.weight_class <> '未知' THEN '同重量级'
                   ELSE '相似'
               END as relation_label
        ORDER BY similarity_score DESC, other.name
        LIMIT $limit
        """
        return self.pool.execute_query(
            query, {"zol_id": zol_id, "limit": limit}, count_as_error=False
        )

    def compare_laptops(self, zol_id_1: str, zol_id_2: str) -> Dict:
        """对比两款笔记本"""
        query = """
        MATCH (l1:Laptop {zol_id: $zol_id_1})
        MATCH (l2:Laptop {zol_id: $zol_id_2})
        OPTIONAL MATCH (l1)-[:BELONGS_TO]->(b1:Brand)
        OPTIONAL MATCH (l2)-[:BELONGS_TO]->(b2:Brand)
        OPTIONAL MATCH (l1)-[:HAS_CPU]->(c1:CPU)
        OPTIONAL MATCH (l2)-[:HAS_CPU]->(c2:CPU)
        OPTIONAL MATCH (l1)-[:HAS_GPU]->(g1:GPU)
        OPTIONAL MATCH (l2)-[:HAS_GPU]->(g2:GPU)
        RETURN {
            laptop1: {name: l1.name, positioning: l1.positioning, weight_class: l1.weight_class,
                       screen_class: l1.screen_class, port_class: l1.port_class,
                       resolution: l1.resolution, refresh_rate: l1.refresh_rate},
            laptop2: {name: l2.name, positioning: l2.positioning, weight_class: l2.weight_class,
                       screen_class: l2.screen_class, port_class: l2.port_class,
                       resolution: l2.resolution, refresh_rate: l2.refresh_rate},
            brand: [b1.name, b2.name],
            cpu: [c1.name, c2.name],
            gpu: [g1.name, g2.name],
            similarity: {
                positioning: CASE WHEN l1.positioning = l2.positioning THEN 1 ELSE 0 END,
                screen: CASE WHEN l1.screen_class = l2.screen_class THEN 1 ELSE 0 END,
                weight: CASE WHEN l1.weight_class = l2.weight_class THEN 1 ELSE 0 END
            }
        } as result
        """
        results = self.pool.execute_query(
            query, {"zol_id_1": zol_id_1, "zol_id_2": zol_id_2}, count_as_error=False
        )
        return results[0]['result'] if results else {}

    def get_same_brand_laptops(self, brand: str, limit: int = 10) -> List[Dict]:
        """同品牌笔记本"""
        query = """
        MATCH (l:Laptop)-[:BELONGS_TO]->(b:Brand {name: $brand})
        RETURN l, b.name as brand
        ORDER BY l.name
        LIMIT $limit
        """
        return self.pool.execute_query(
            query, {"brand": brand, "limit": limit}, count_as_error=False
        )

    def get_laptops_by_positioning(self, positioning: str, limit: int = 10) -> List[Dict]:
        """按产品定位查找笔记本（游戏本/轻薄本/商务本等）"""
        query = """
        MATCH (l:Laptop)
        WHERE l.positioning CONTAINS $positioning
        OPTIONAL MATCH (l)-[:BELONGS_TO]->(b:Brand)
        RETURN l, b.name as brand
        ORDER BY l.name
        LIMIT $limit
        """
        return self.pool.execute_query(
            query, {"positioning": positioning, "limit": limit}, count_as_error=False
        )


if __name__ == "__main__":
    print("🚀 初始化连接池...")
    pool = dbdriver.get_connection_pool(
        "bolt://localhost:7687",
        "neo4j",
        "yangboneo4j",
        max_pool_size=10
    )

    print("📊 连接池状态:")
    print(pool.get_pool_status())
    print()

    graphrag = GraphRAGQueries(pool)

    # ==================== 1. 按名称搜索实体（手机/电脑/品牌都支持） ====================
    print("=" * 60)
    print("🔍 按名称搜索实体")
    print("=" * 60)

    # 搜索手机
    print("\n📱 搜索手机 'iPhone':")
    results = graphrag.search_entities("iPhone", limit=5)
    if results:
        for i, r in enumerate(results, 1):
            e = r.get('e', {})
            labels = e.get('labels', e.labels) if hasattr(e, 'labels') else []
            print(f"  {i}. {e.get('name', 'N/A')} | 品牌: {e.get('brand', 'N/A')} | 价格: {e.get('jd_price', 'N/A')}")
    else:
        print("  ❌ 没有找到结果")

    # 搜索电脑
    print("\n💻 搜索电脑 'MacBook':")
    results = graphrag.search_entities("MacBook", limit=5)
    if results:
        for i, r in enumerate(results, 1):
            e = r.get('e', {})
            print(f"  {i}. {e.get('name', 'N/A')} | 品牌: {e.get('brand', 'N/A')}")
    else:
        print("  ❌ 没有找到结果")

    # 搜索品牌
    print("\n🏷️ 搜索品牌 '华为':")
    results = graphrag.search_entities("华为", limit=5)
    if results:
        for i, r in enumerate(results, 1):
            e = r.get('e', {})
            print(f"  {i}. {e.get('name', 'N/A')} | 类型: {e.get('brand', 'N/A')}")
    else:
        print("  ❌ 没有找到结果")

    # ==================== 2. 手机关联推荐（先按名称找到手机） ====================
    print("\n" + "=" * 60)
    print("📱 手机关联推荐")
    print("=" * 60)

    # 先按名称搜索到手机，获取其 phone_id
    search_result = graphrag.search_entities("iPhone 16 Pro", limit=1)
    if search_result:
        phone = search_result[0].get('e', {})
        phone_name = phone.get('name', 'N/A')
        phone_id = phone.get('phone_id', '')
        print(f"\n📍 找到手机: {phone_name} (ID: {phone_id})")

        relations = graphrag.get_phone_relationships(phone_id, limit=5)
        for r in relations:
            related = r.get('related_phone', {})
            label = r.get('relation_label', '未知')
            score = r.get('similarity_score', 0)
            print(f"  - {related.get('name')} ({label}) 相似度: {score}")
    else:
        print("  ❌ 未找到该手机")

    # ==================== 3. 价格范围查询 ====================
    print("\n" + "=" * 60)
    print("💰 价格范围查询 (3000-6000)")
    print("=" * 60)
    phones = graphrag.get_phones_by_price_range(3000, 6000)
    for i, r in enumerate(phones[:5], 1):
        p = r.get('p', {})
        print(f"  {i}. {p.get('name', 'N/A')} - {p.get('jd_price', 'N/A')}")

    # ==================== 4. 同品牌手机 ====================
    print("\n" + "=" * 60)
    print("🏷️ 同品牌手机 (苹果)")
    print("=" * 60)
    same_brand = graphrag.get_similar_phones_by_brand("苹果", limit=5)
    for r in same_brand:
        p = r.get('p', {})
        price = r.get('price', 0)
        print(f"  - {p.get('name')} ¥{price}")

    print("\n📊 最终连接池状态:")
    print(pool.get_pool_status())
    print()

    pool.close()
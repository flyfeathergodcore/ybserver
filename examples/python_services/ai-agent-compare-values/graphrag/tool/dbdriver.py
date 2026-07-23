import neo4j
from queue import Queue, Empty
from threading import Lock
from contextlib import contextmanager
import time
import logging
from typing import Dict, Any, List, Optional
from dataclasses import dataclass
from datetime import datetime

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

@dataclass
class ConnectionPoolStats:
    """连接池统计信息"""
    current_size: int
    max_pool_size: int
    min_pool_size: int
    available_connections: int
    in_use: int
    is_closed: bool
    total_queries: int = 0
    total_errors: int = 0
    created_at: str = None
    
    def to_dict(self):
        return {
            'current_size': self.current_size,
            'max_pool_size': self.max_pool_size,
            'min_pool_size': self.min_pool_size,
            'available_connections': self.available_connections,
            'in_use': self.in_use,
            'is_closed': self.is_closed,
            'total_queries': self.total_queries,
            'total_errors': self.total_errors,
            'created_at': self.created_at
        }

class Neo4jConnectionPool:
    def __init__(self, uri: str, user: str, password: str, 
                 max_pool_size: int = 10, min_pool_size: int = 2,
                 database: str = "neo4j", connection_timeout: int = 30):
        """
        自定义 Neo4j 连接池（专门为 GraphRAG 优化）
        
        Args:
            uri: Neo4j 数据库URI
            user: 用户名
            password: 密码
            max_pool_size: 最大连接数
            min_pool_size: 最小连接数（初始连接数）
            database: 数据库名称
            connection_timeout: 连接超时时间（秒）
        """
        self.uri = uri
        self.user = user
        self.password = password
        self.max_pool_size = max_pool_size
        self.min_pool_size = min_pool_size
        self.database = database
        self.connection_timeout = connection_timeout
        
        self._pool = Queue(maxsize=max_pool_size)
        self._current_size = 0
        self._lock = Lock()
        self._is_closed = False
        
        # 统计信息
        self._total_queries = 0
        self._total_errors = 0
        self._created_at = datetime.now().isoformat()
        
        # 初始化最小连接数
        self._initialize_pool()
    
    def _initialize_pool(self):
        """初始化连接池，创建最小数量的连接"""
        for i in range(self.min_pool_size):
            try:
                driver = self._create_driver()
                self._pool.put(driver)
                with self._lock:
                    self._current_size += 1
                logger.info(f"✅ 创建初始连接 {i+1}/{self.min_pool_size}")
            except Exception as e:
                logger.error(f"❌ 创建初始连接失败: {e}")
    
    def _create_driver(self):
        """创建一个新的驱动连接"""
        return neo4j.GraphDatabase.driver(
            self.uri, 
            auth=(self.user, self.password),
            max_connection_pool_size=1,
            connection_timeout=self.connection_timeout,
        )
    
    def _create_new_connection(self):
        """创建新连接（带锁保护）"""
        with self._lock:
            if self._current_size < self.max_pool_size and not self._is_closed:
                try:
                    driver = self._create_driver()
                    self._current_size += 1
                    logger.info(f"🔗 创建新连接，当前连接数: {self._current_size}")
                    return driver
                except Exception as e:
                    logger.error(f"❌ 创建新连接失败: {e}")
                    self._total_errors += 1
                    return None
            return None
    
    def get_connection(self, timeout: int = 30):
        """
        从连接池获取连接
        
        Args:
            timeout: 超时时间（秒）
        """
        if self._is_closed:
            raise Exception("连接池已关闭")
        
        try:
            driver = self._pool.get(timeout=timeout)
            if not self._is_connection_valid(driver):
                logger.warning("⚠️ 连接无效，创建新连接")
                driver.close()
                with self._lock:
                    self._current_size -= 1
                driver = self._create_new_connection()
                if driver is None:
                    raise Exception("无法创建新连接")
            return driver
        except Empty:
            logger.warning("⚠️ 连接池为空，尝试创建新连接")
            driver = self._create_new_connection()
            if driver is None:
                raise Exception(f"获取连接超时，池大小: {self._current_size}")
            return driver
    
    def _is_connection_valid(self, driver):
        """验证连接是否有效"""
        try:
            with driver.session(database=self.database) as session:
                session.run("RETURN 1").data()
            return True
        except Exception as e:
            logger.debug(f"连接验证失败: {e}")
            return False
    
    def return_connection(self, driver):
        """归还连接到连接池"""
        if self._is_closed:
            driver.close()
            with self._lock:
                self._current_size -= 1
            return
        
        if driver and self._is_connection_valid(driver):
            try:
                self._pool.put(driver, timeout=5)
            except Queue.Full:
                logger.warning("⚠️ 连接池已满，关闭连接")
                driver.close()
                with self._lock:
                    self._current_size -= 1
        else:
            if driver:
                logger.warning("⚠️ 连接无效，关闭连接")
                driver.close()
                with self._lock:
                    self._current_size -= 1
    
    @contextmanager
    def get_session(self):
        """获取会话（上下文管理器）"""
        driver = self.get_connection()
        session = driver.session(database=self.database)
        try:
            yield session
        except Exception as e:
            logger.error(f"❌ 会话执行异常: {e}")
            self._total_errors += 1
            raise
        finally:
            session.close()
            self.return_connection(driver)
    
    def execute_query(self, query: str, parameters: Dict = None, 
                      retry: int = 3, timeout: int = 30,
                      count_as_error: bool = True) -> List[Dict]:
        """
        执行查询并返回结果（带重试机制）
        
        Args:
            query: Cypher 查询语句
            parameters: 查询参数
            retry: 重试次数
            timeout: 超时时间（秒）
            count_as_error: 是否将失败计入错误统计
        
        Returns:
            查询结果列表
        """
        parameters = parameters or {}
        
        for attempt in range(retry):
            try:
                with self.get_session() as session:
                    result = session.run(query, parameters, timeout=timeout)
                    data = result.data()
                    self._total_queries += 1
                    logger.debug(f"📊 查询执行成功，返回 {len(data)} 条记录")
                    return data
            except neo4j.exceptions.ServiceUnavailable as e:
                logger.warning(f"⚠️ 服务不可用 (尝试 {attempt+1}/{retry}): {e}")
                if attempt == retry - 1:
                    raise
                time.sleep(2 ** attempt)
            except neo4j.exceptions.SessionExpired as e:
                logger.warning(f"⚠️ 会话过期 (尝试 {attempt+1}/{retry}): {e}")
                if attempt == retry - 1:
                    raise
                time.sleep(1)
            except Exception as e:
                if count_as_error:
                    self._total_errors += 1
                logger.warning(f"⚠️ 查询执行失败 (尝试 {attempt+1}/{retry}): {e}")
                if attempt == retry - 1:
                    raise
                time.sleep(0.5)
    
    def execute_transaction(self, query: str, parameters: Dict = None) -> List[Dict]:
        """执行事务查询"""
        parameters = parameters or {}
        with self.get_session() as session:
            def work(tx):
                result = tx.run(query, parameters)
                return result.data()
            try:
                self._total_queries += 1
                return session.execute_write(work)
            except Exception as e:
                self._total_errors += 1
                raise
    
    def batch_execute(self, queries: List[tuple]) -> List[Any]:
        """批量执行查询"""
        results = []
        with self.get_session() as session:
            for query, params in queries:
                try:
                    result = session.run(query, params or {})
                    results.append(result.data())
                except Exception as e:
                    logger.error(f"❌ 批量查询失败: {e}")
                    results.append(None)
        return results
    
    def close(self):
        """关闭连接池"""
        self._is_closed = True
        closed_count = 0
        while not self._pool.empty():
            try:
                driver = self._pool.get_nowait()
                driver.close()
                with self._lock:
                    self._current_size -= 1
                    closed_count += 1
            except Empty:
                break
        logger.info(f"🔒 连接池已关闭，关闭连接数: {closed_count}，剩余连接数: {self._current_size}")
    
    def get_pool_status(self) -> Dict[str, Any]:
        """获取连接池状态"""
        return {
            'uri': self.uri,
            'database': self.database,
            'current_size': self._current_size,
            'max_pool_size': self.max_pool_size,
            'min_pool_size': self.min_pool_size,
            'available_connections': self._pool.qsize(),
            'in_use': self._current_size - self._pool.qsize(),
            'is_closed': self._is_closed,
            'total_queries': self._total_queries,
            'total_errors': self._total_errors,
            'created_at': self._created_at
        }
    
    def health_check(self) -> bool:
        """健康检查"""
        try:
            result = self.execute_query("RETURN 1 as test", retry=1)
            return result and result[0].get('test') == 1
        except Exception as e:
            logger.error(f"❌ 健康检查失败: {e}")
            return False


# 单例模式（全局连接池）
_global_pool: Optional[Neo4jConnectionPool] = None

def get_connection_pool(uri: str = "bolt://localhost:7687", 
                       user: str = "neo4j", 
                       password: str = "password",
                       max_pool_size: int = 10) -> Neo4jConnectionPool:
    """获取全局连接池（单例模式）"""
    global _global_pool
    if _global_pool is None or _global_pool._is_closed:
        _global_pool = Neo4jConnectionPool(uri, user, password, max_pool_size)
    return _global_pool


class GraphRAGQueries:
    """GraphRAG 相关的查询"""
    
    def __init__(self, pool: Neo4jConnectionPool):
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


if __name__ == "__main__":
    print("🚀 初始化连接池...")
    pool = get_connection_pool(
        "bolt://localhost:7687",
        "neo4j",
        "yangboneo4j",
        max_pool_size=10
    )
    
    print("📊 连接池状态:")
    print(pool.get_pool_status())
    print()
    
    graphrag = GraphRAGQueries(pool)
    
    # 1. 获取特定手机的关联推荐
    print("📱 获取手机关联推荐:")
    phone_id = "2139583"
    
    relations = graphrag.get_phone_relationships(phone_id, limit=5)
    for r in relations:
        phone = r.get('related_phone', {})
        label = r.get('relation_label', '未知')
        score = r.get('similarity_score', 0)
        print(f"  - {phone.get('name')} ({label}) 相似度: {score}")
    print()
    
    # 2. 获取竞争机型
    print("⚔️ 竞争机型:")
    competitors = graphrag.get_competitive_phones(phone_id, limit=5)
    for r in competitors:
        phone = r.get('related_phone', {})
        level = r.get('competitive_level', '未知')
        print(f"  - {phone.get('name')} ({level})")
    print()
    
    # 3. 对比两个手机
    print("📊 对比两个手机:")
    comparison = graphrag.get_phone_comparison(phone_id, "2139584")
    if comparison:
        print(f"  相似度: {comparison['similarity']['total'] * 100:.1f}%")
        print(f"  价格差: ¥{comparison['comparison']['price_diff']}")
        print(f"  品牌: {comparison['comparison']['brand']}")
    print()
    
    # 4. 获取同品牌手机
    print("🏷️ 同品牌手机:")
    same_brand = graphrag.get_similar_phones_by_brand("苹果", limit=5)
    for r in same_brand:
        phone = r.get('p', {})
        price = r.get('price', 0)
        print(f"  - {phone.get('name')} ¥{price}")
    print()
    
    # 5. 搜索测试
    print("🔍 搜索测试:")
    try:
        results = graphrag.search_entities("iPhone", limit=5)
        if results:
            for i, result in enumerate(results, 1):
                entity = result.get('e', {})
                print(f"  {i}. {entity.get('name', 'N/A')} - {entity.get('brand', 'N/A')}")
        else:
            print("  ❌ 没有找到结果")
    except Exception as e:
        print(f"  ❌ 搜索失败: {e}")
    print()
    
    # 6. 价格范围查询
    print("💰 价格范围查询 (3000-6000):")
    try:
        phones = graphrag.get_phones_by_price_range(3000, 6000)
        for i, result in enumerate(phones[:5], 1):
            phone_data = result.get('p', {})
            print(f"  {i}. {phone_data.get('name', 'N/A')} - {phone_data.get('jd_price', 'N/A')}")
    except Exception as e:
        print(f"  ❌ 查询失败: {e}")
    print()
    
    # 7. 属性查询
    print("📱 查询手机 (品牌=苹果):")
    try:
        phone = graphrag.get_phone_by_attributes({"brand": "苹果"})
        for i, result in enumerate(phone[:3], 1):
            phone_data = result.get('p', {})
            print(f"  {i}. {phone_data.get('name')} - {phone_data.get('jd_price')}")
    except Exception as e:
        print(f"  ❌ 查询失败: {e}")
    print()
    
    print("📊 最终连接池状态:")
    print(pool.get_pool_status())
    print()
    
    pool.close()
import os
import logging
import json
import threading
import time
from typing import Optional, Dict, List, Tuple
from datetime import datetime, timedelta
from functools import wraps
from openai import OpenAI
from dotenv import load_dotenv

load_dotenv()
logger = logging.getLogger(__name__)

class EmbeddingCategoryMatcher:
    """Embedding 品类匹配器"""
    
    def __init__(self, config_path: Optional[str] = None):
        self.config_path = config_path or "categories.json"
        self._lock = threading.Lock()
        self._cache = None
        self._cache_timestamp = None
        self._cache_ttl = timedelta(hours=24)
        self._load_config()
        self._init_embedding_cache()
    
    def _load_config(self):
        """加载品类配置"""
        default_config = {
            "categories": {
                "笔记本电脑": ["笔记本", "电脑", "显示器", "键盘", "鼠标", "笔记本电脑", "台式机"],
                "手机": ["iPhone", "android", "手机", "智能手机", "华为", "小米", "苹果"],
                "平板": ["iPad", "surface", "平板电脑", "ipad"],
                "耳机": ["音响", "音箱", "earphone", "耳机", "蓝牙耳机", "无线耳机"],
                "相机": ["相机", "单反", "微单", "数码相机", "胶片相机"],
                "音响": ["音响", "音箱", "功放", "音响系统"],
                "智能手表": ["手表", "智能手表", "apple watch", "小米手环"],
                "游戏机": ["游戏机", "主机", "switch", "playstation", "xbox"],
                "电视": ["电视", "智能电视", "led电视", "oled电视"],
                "路由器": ["路由器", "wifi", "网络", "宽带路由器"],
                "键盘": ["键盘", "机械键盘", "薄膜键盘"],
                "鼠标": ["鼠标", "无线鼠标", "游戏鼠标"],
                "显示器": ["显示器", "屏幕", "lcd显示器", "led显示器"],
                "笔记本电脑": ["笔记本电脑", "笔记本", "便携电脑", "超极本"],
                "耳机": ["耳机", "耳麦", "头戴式耳机", "入耳式耳机"]
            },
            "prompts": {
                "笔记本电脑": "请告诉我您对笔记本电脑的需求：\n- 处理器类型（Intel/AMD）\n- 内存大小（8GB/16GB/32GB）\n- 存储容量（256GB/512GB/1TB）\n- 屏幕尺寸（13寸/15寸/17寸）\n- 是否需要轻薄便携性",
                "手机": "请告诉我您对手机的需求：\n- 存储容量（64GB/128GB/256GB）\n- 拍照像素（12MP/48MP/108MP）\n- 电池容量（3000mAh/4000mAh/5000mAh）\n- 操作系统（iOS/Android）\n- 品牌偏好",
                "平板": "请告诉我您对平板的需求：\n- 存储容量（64GB/128GB/256GB）\n- 屏幕尺寸（10寸/11寸/12寸）\n- 是否需要随身携带\n- 主要用途（娱乐/办公/学习）",
                "耳机": "请告诉我您对耳机的需求：\n- 驱动方式（有线/无线）\n- 使用场景（办公/游戏/运动）\n- 音质偏好（音乐/低音）\n- 品牌偏好\n- 是否需要主动降噪",
                "相机": "请告诉我您对相机的需求：\n- 机身类型（单反/微单/便携）\n- 像素要求（20MP/40MP/60MP）\n- 镜头类型（定焦/变焦）\n- 品牌偏好\n- 是否需要视频拍摄能力",
                "音响": "请告诉我您对音响的需求：\n- 使用场景（家庭/户外）\n- 功率要求（10W/50W/100W）\n- 音响类型（落地/桌面/便携）\n- 品牌偏好\n- 是否需要低音炮",
                "智能手表": "请告诉我您对智能手表的需求：\n- 使用场景（运动/日常）\n- 续航要求（1天/2天/3天）\n- 功能要求（健康监测/消息通知）\n- 品牌偏好\n- 是否需要防水",
                "游戏机": "请告诉我您对游戏机的需求：\n- 游戏类型偏好（动作/角色/赛车）\n- 是否需要3D游戏\n- 品牌偏好\n- 游戏平台要求（PS5/Xbox/NS）\n- 预算范围",
                "电视": "请告诉我您对电视的需求：\n- 屏幕尺寸（32寸/43寸/55寸/65寸）\n- 分辨率要求（1080p/4K/8K）\n- 使用场景（客厅/卧室）\n- 品牌偏好\n- 是否需要智能系统",
                "路由器": "请告诉我您对路由器的需求：\n- 使用场景（家庭/办公）\n- 网络要求（千兆/万兆）\n- 覆盖范围（单层/多层）\n- 品牌偏好\n- 频段支持要求（2.4GHz/5GHz/6GHz）",
                "键盘": "请告诉我您对键盘的需求：\n- 键盘类型（机械/薄膜）\n- 使用场景（办公/游戏）\n- 是否需要背光\n- 品牌偏好\n- 尺寸偏好（全尺寸/TKL）",
                "鼠标": "请告诉我您对鼠标的需求：\n- 使用场景（办公/游戏）\n- 鼠标类型（光学/激光）\n- 是否需要无线\n- 品牌偏好\n- 灵敏度要求",
                "显示器": "请告诉我您对显示器的需求：\n- 屏幕尺寸（24寸/27寸/32寸）\n- 分辨率要求（1080p/4K）\n- 刷新率（60Hz/144Hz/165Hz）\n- 使用场景（办公/游戏）\n- 品牌偏好",
                "笔记本电脑": "请告诉我您对笔记本电脑的需求：\n- 处理器类型（Intel/AMD）\n- 内存大小（8GB/16GB/32GB）\n- 存储容量（256GB/512GB/1TB）\n- 屏幕尺寸（13寸/15寸/17寸）\n- 是否需要轻薄便携性",
                "耳机": "请告诉我您对耳机的需求：\n- 驱动方式（有线/无线）\n- 使用场景（办公/游戏/运动）\n- 音质偏好（音乐/低音）\n- 品牌偏好\n- 是否需要主动降噪"
            }
        }
        
        if os.path.exists(self.config_path):
            with open(self.config_path, 'r', encoding='utf-8') as f:
                config = json.load(f)
                self.category_mapping = config.get('categories', {})
                self.prompts = config.get('prompts', {})
        else:
            self.category_mapping = default_config['categories']
            self.prompts = default_config['prompts']
            self._save_config()
        
        # 构建反向映射
        self.keyword_to_category = {}
        for category, keywords in self.category_mapping.items():
            for keyword in keywords:
                self.keyword_to_category[keyword] = category
    
    def _save_config(self):
        """保存配置"""
        with open(self.config_path, 'w', encoding='utf-8') as f:
            json.dump({
                'categories': self.category_mapping,
                'prompts': self.prompts
            }, f, ensure_ascii=False, indent=2)
    
    def _get_embedding_client(self):
        """获取 embedding 客户端"""
        api_key = os.getenv("EMBEDDING_API_KEY")
        base_url = os.getenv("EMBEDDING_BASE_URL")
        
        if not api_key or not base_url:
            logger.error("缺少 EMBEDDING_API_KEY 或 EMBEDDING_BASE_URL 环境变量")
            return None
        
        return OpenAI(api_key=api_key, base_url=base_url, timeout=30)
    
    def _init_embedding_cache(self):
        """初始化嵌入向量缓存"""
        with self._lock:
            if self._cache and self._is_cache_valid():
                return
            
            client = self._get_embedding_client()
            if not client:
                return
            
            try:
                category_names = list(self.category_mapping.keys())
                response = client.embeddings.create(
                    model="BAAI/bge-m3",
                    input=category_names
                )
                
                self._cache = {
                    'vectors': {},
                    'norms': {},
                    'names': category_names
                }
                
                for i, name in enumerate(category_names):
                    vector = response.data[i].embedding
                    norm = sum(v*v for v in vector) ** 0.5
                    self._cache['vectors'][name] = vector
                    self._cache['norms'][name] = norm
                
                self._cache_timestamp = datetime.now()
                logger.info(f"成功初始化 {len(category_names)} 个品类的 embedding 缓存")
                
            except Exception as e:
                logger.error(f"初始化 embedding 缓存失败: {e}")
    
    def _is_cache_valid(self) -> bool:
        """检查缓存是否有效"""
        if self._cache_timestamp is None:
            return False
        return datetime.now() - self._cache_timestamp < self._cache_ttl
    
    def _calculate_similarity(self, vec1: List[float], norm1: float, 
                             vec2: List[float], norm2: float) -> float:
        """计算余弦相似度"""
        if norm1 == 0 or norm2 == 0:
            return 0
        
        dot_product = sum(a*b for a, b in zip(vec1, vec2))
        return dot_product / (norm1 * norm2)
    
    def get_category(self, product: str) -> Optional[str]:
        """获取产品品类"""
        # 1. 关键词精确匹配
        product_lower = product.lower()
        for keyword, category in self.keyword_to_category.items():
            if keyword in product_lower:
                return category
        
        # 2. Embedding 相似度匹配
        client = self._get_embedding_client()
        if client and self._cache:
            try:
                # 生成产品 embedding
                response = client.embeddings.create(
                    model="BAAI/bge-m3",
                    input=product
                )
                product_vector = response.data[0].embedding
                product_norm = sum(v*v for v in product_vector) ** 0.5
                
                # 找最相似的品类
                best_category = None
                best_similarity = -1
                
                for category, vector in self._cache['vectors'].items():
                    similarity = self._calculate_similarity(
                        product_vector, product_norm,
                        vector, self._cache['norms'][category]
                    )
                    
                    if similarity > best_similarity:
                        best_similarity = similarity
                        best_category = category
                
                # 设置相似度阈值
                if best_similarity > 0.5:
                    logger.info(f"通过 embedding 匹配到品类: {best_category} (相似度: {best_similarity:.3f})")
                    return best_category
                
            except Exception as e:
                logger.warning(f"Embedding 匹配失败: {e}")
        
        # 3. 默认返回
        logger.warning(f"无法匹配产品: {product}，使用默认品类")
        return "笔记本电脑"
    
    def get_prompt(self, product: str) -> str:
        """获取产品引导词"""
        category = self.get_category(product)
        return self.prompts.get(category, "请告诉我您想要的商品类型")

# 全局实例
_matcher = None

def get_matcher() -> EmbeddingCategoryMatcher:
    """获取匹配器单例"""
    global _matcher
    if _matcher is None:
        _matcher = EmbeddingCategoryMatcher()
    return _matcher

# 保持原有 API 兼容
def get_product_category_with_embedding(product: str) -> Optional[str]:
    return get_matcher().get_category(product)

def get_product_prompt(product: str) -> str:
    return get_matcher().get_prompt(product)


import sys, os
_project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, _project_root)

import logging
from mcp.server.fastmcp import FastMCP
from depend.db import get_conn as _get_raw_conn
from depend.auth import require_role
from depend.get_time import get_now_datetime_str
from typing import Optional
import json, time
import mysql.connector as connector
import jd.api
import jd
from urllib.parse import quote, unquote

logger = logging.getLogger(__name__)
logger.info("模块加载完成，项目根目录: %s", _project_root)


def _get_conn():
    """从配置创建 MySQL 连接并确保 product 相关表存在"""
    conn = _get_raw_conn()
    cur = conn.cursor()
    cur.execute("""CREATE TABLE IF NOT EXISTS products (
        product_id INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '商品自增主键',
        jd_sku VARCHAR(32) NOT NULL COMMENT '京东SKU编码',
        product_name VARCHAR(255) NOT NULL DEFAULT '' COMMENT '商品名称',
        current_price INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '当前最低价格(单位：分)',
        min_platform VARCHAR(32) NOT NULL COMMENT '最低价所属平台',
        url VARCHAR(1024) NOT NULL DEFAULT '' COMMENT '商品链接',
        image_url VARCHAR(1024) NOT NULL DEFAULT '' COMMENT '商品主图链接',
        parameters JSON DEFAULT NULL COMMENT '商品参数',
        create_at DATETIME(3) NOT NULL COMMENT '商品录入时间',
        update_at DATETIME(3) NOT NULL COMMENT '商品信息更新时间',
        PRIMARY KEY (product_id),
        UNIQUE KEY uk_jd_sku (jd_sku)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""")
    cur.execute("""CREATE TABLE IF NOT EXISTS product_price_history (
        id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '历史记录自增主键',
        jd_sku VARCHAR(32) NOT NULL COMMENT '关联商品SKU',
        price INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '本次抓取价格(单位：分)',
        platform VARCHAR(32) NOT NULL COMMENT '价格所属平台',
        capture_time DATETIME(3) NOT NULL COMMENT '价格抓取时间',
        PRIMARY KEY (id),
        INDEX idx_sku_time (jd_sku, capture_time DESC),
        CONSTRAINT fk_sku_ref_product FOREIGN KEY (jd_sku) REFERENCES products(jd_sku) ON DELETE CASCADE ON UPDATE CASCADE
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""")
    conn.commit()
    cur.close()
    return conn


def _upsert_product(conn, p: dict):
    """插入或更新 product 记录（按 jd_sku 去重）"""
    cur = conn.cursor()
    now = get_now_datetime_str()
    cur.execute("""INSERT INTO products (jd_sku, product_name, current_price, min_platform, url, image_url, parameters, create_at, update_at)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON DUPLICATE KEY UPDATE
            product_name=VALUES(product_name),
            current_price=VALUES(current_price),
            min_platform=VALUES(min_platform),
            url=VALUES(url),
            image_url=VALUES(image_url),
            parameters=VALUES(parameters),
            update_at=VALUES(update_at)""",
        (p["jd_sku"], p["product_name"], p.get("current_price", 0),
         p.get("min_platform", "jd"), p.get("url", ""), p.get("image_url", ""),
         json.dumps(p.get("parameters")) if p.get("parameters") else None,
         p.get("create_at", now), p.get("update_at", now)))
    conn.commit()


def _record_price(conn, jd_sku: str, price: int, platform: str = ""):
    """记录一条价格到 product_price_history（price 单位：分）"""
    cur = conn.cursor()
    cur.execute("""INSERT INTO product_price_history (jd_sku, price, platform, capture_time)
        VALUES (%s, %s, %s, %s)""",
        (jd_sku, price, platform, get_now_datetime_str()))
    conn.commit()


def register_skill(fastmcp: FastMCP) -> None:
    """注册产品agent的skill到FastMCP框架中。"""
    try:
        app_key = os.getenv("JD_APP_KEY")
        app_secret = os.getenv("JD_APP_SECRET")
        if not app_key or not app_secret:
            raise ValueError("JD_APP_KEY 和 JD_APP_SECRET 环境变量必须设置")
        jd.setDefaultAppInfo(app_key, app_secret)
        logger.info("JD SDK 初始化完成")
    except Exception as e:
        import traceback
        logger.error("JD SDK 初始化失败: %s", e, exc_info=True)

    @fastmcp.tool()
    def search_category(keyword: str, price_min: int, price_max: int,
                        sort_type: Optional[str] = "sort_redissale_desc",
                        page: int = 1, limit: int = 20) -> list[dict]:
        """
        通过京东开放平台搜索商品，返回真实商品列表并同步存入数据库。

        Args:
            keyword: 搜索关键词，如"华为手机"、"小米平板"
            price_min: 最低价格（单位：元），如 2600 表示 2600 元
            price_max: 最高价格（单位：元），如 2800 表示 2800 元
            sort_type: 排序方式
                       sort_redissale_desc    — 销量降序（默认）
                       sort_dredisprice_asc   — 价格升序
                       sort_dredisprice_desc  — 价格降序
            page: 页码，从 1 开始，默认 1
            limit: 单页返回条数，默认 20

        Returns:
            商品列表，每项包含：
              - wareid: 商品 ID
              - name:   商品名称（URL 解码后）
              - image:  图片 URL
              - good:   好评率（百分比数值）
              - catid:  类目 ID
              - cid1/cid2: 一级/二级类目
              - shop_id: 店铺 ID
        """
        logger.info("搜索商品: keyword=%s price=%d~%d sort=%s page=%d",
                     keyword, price_min, price_max, sort_type, page)
        request = jd.api.SearchWareRequest()
        request.key = quote(keyword)
        request.filt_type = f"dredisprice,L{price_max}M{price_min}"
        request.sort_type = sort_type
        request.page = str(page)
        request.charset = "utf-8"
        request.urlencode = "no"

        try:
            resp = request.getResponse()
        except Exception as e:
            logger.error("搜索商品 API 调用失败: %s", e)
            return []

        result = resp.get("jingdong_search_ware_responce", {})
        para = result.get("Paragraph", [])
        if isinstance(para, str):
            try:
                para = json.loads(para)
            except json.JSONDecodeError:
                logger.error("搜索响应 json 解析失败")
                return []

        # 尝试连接数据库
        conn = None
        try:
            conn = _get_conn()
        except Exception as e:
            logger.warning("数据库连接失败，不保存结果: %s", e)

        products = []
        for p in para[:limit]:
            content = p.get("Content", {})
            wareid = p.get("wareid", "")
            name = unquote(content.get("warename", ""))
            image = content.get("imageurl", "")
            if image and not image.startswith("http"):
                image = f"https://img14.360buyimg.com/n0/{image}"

            product = {
                "wareid": p.get("wareid"),
                "name": name,
                "image": content.get("imageurl"),
                "good": p.get("good"),
                "catid": p.get("catid"),
                "cid1": p.get("cid1"),
                "cid2": p.get("cid2"),
                "shop_id": p.get("shop_id"),
            }
            products.append(product)

            # 存入 MySQL
            if conn and wareid:
                try:
                    _upsert_product(conn, {
                        "jd_sku": wareid,
                        "product_name": name,
                        "url": f"https://item.jd.com/{wareid}.html",
                        "image_url": image,
                    })
                except Exception as e:
                    logger.warning("保存商品 %s 失败: %s", wareid, e)

        if conn:
            conn.close()

        logger.info("搜索商品完成: 关键词=%s, 返回 %d 条", keyword, len(products))
        return products

    @fastmcp.tool()
    def find_price(products: list[str]) -> dict:
        """
        通过慢慢买比价网站爬取指定商品的最新价格，结果自动存入价格历史表。

        Args:
            products: 商品名称列表，如 ["华为 Mate 60", "iPhone 15", "小米14"]
                      每个名称会独立搜索，间隔约 1.5 秒避免被反爬

        Returns:
            dict，格式 {"products": {商品名: [
                {"title": "完整标题", "price": "价格字符串", "platform": "来源平台", "url": "商品链接"}
            ]}}
            查询失败的商品对应值为 {"error": "错误描述"}
        """
        from depend.manmanbuy_price import handle as price_handle

        logger.info("查询价格: %d 个商品", len(products))
        all_prices = {}
        conn = None
        try:
            conn = _get_conn()
        except Exception as e:
            logger.warning("数据库连接失败，不保存价格: %s", e)

        for i, product in enumerate(products):
            # 每次请求间隔 1~2 秒，避免被反爬
            if i > 0:
                time.sleep(1.5)

            try:
                result = price_handle(product)
                if result.get("success"):
                    items = result.get("products", [])
                    price_list = []
                    for item in items:
                        entry = {
                            "title": item.get("title"),
                            "price": item.get("price"),
                            "platform": item.get("platform"),
                            "url": item.get("url"),
                        }
                        price_list.append(entry)

                        # 存入 product_price_history
                        if conn and item.get("price"):
                            try:
                                price_fen = int(float(item["price"]) * 100)
                                _record_price(
                                    conn,
                                    jd_sku=item.get("title", product),
                                    price=price_fen,
                                    platform=item.get("platform", "unknown"),
                                )
                            except Exception as e:
                                logger.warning("保存价格失败: %s", e)

                    all_prices[product] = price_list
                    logger.info("  价格查询成功: %s → %d 条结果", product, len(price_list))
                else:
                    all_prices[product] = {"error": result.get("error", "查询失败")}
                    logger.warning("  价格查询失败: %s → %s", product, result.get("error"))
            except Exception as e:
                all_prices[product] = {"error": str(e)}
                logger.error("  价格查询异常 [%s]: %s", product, e)

        if conn:
            conn.close()

        logger.info("价格查询完成: %d/%d 个商品成功",
                     sum(1 for v in all_prices.values() if isinstance(v, list)), len(products))
        return {"products": all_prices}

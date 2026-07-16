import sys, os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

import logging
from mcp.server.fastmcp import FastMCP
from depend.db import load_config, get_conn
from depend.get_time import get_now_datetime_str
from depend.auth import require_role
import mysql.connector as connector
from typing import Optional,Literal
from time import time

logger = logging.getLogger(__name__)


def register_skill(fastmcp: FastMCP)->None:
    """
    注册guide技能到FastMCP框架中。
    """
    @fastmcp.tool()
    def find_product_prompt(product: str) -> str:
        """
        根据用户输入的商品类型生成需求引导词，帮助用户明确选购需求和预算范围。

        Args:
            product: 商品类型名称，支持手机/笔记本电脑/平板/耳机等

        Returns:
            针对该商品类型的引导问题字符串，引导用户补充预算、用途、偏好等信息
        """
        match product:
            case "笔记本电脑":
                return "请告诉我您对笔记本电脑的需求：\n- 预算范围\n- 主要用途（办公/游戏/编程/设计）\n- 屏幕尺寸偏好\n- 品牌偏好"
            case "手机":
                return "请告诉我您对手机的需求：\n- 预算范围\n- 操作系统偏好（iOS/Android）\n- 拍照要求\n- 电池续航要求"
            case "平板":
                return "请告诉我您对平板的需求：\n- 预算范围\n- 主要用途（学习/娱乐/办公）\n- 屏幕尺寸偏好\n- 是否需要手写笔"
            case "耳机":
                return "请告诉我您对耳机的需求：\n- 预算范围\n- 类型（头戴式/入耳式）\n- 连接方式（有线/无线）\n- 主动降噪需求"
            case _:
                return "请告诉我您想要的商品类型，例如：笔记本电脑、手机、平板或耳机。"

    @fastmcp.tool()
    def load_session(session_id: str, status_code: int = 0) -> list:
        """
        从 sessions 表加载指定会话的历史记录。

        Args:
            session_id: 会话唯一标识
            status_code: 会话状态码，与 stage 共同定位唯一记录

        Returns:
            匹配的 session 记录列表（按 create_at 升序），每条包含
            session_id/stage/status_code/content/create_at 等字段；
            未找到或出错时返回 None
        """
        conn = None
        cursor = None
        try:
            conn = get_conn()
            cursor = conn.cursor(dictionary=True)
            cursor.execute(
                "SELECT * FROM sessions WHERE session_id = %s and status_code = %s ORDER BY create_at ASC",
                (session_id,status_code)
            )
            result = cursor.fetchall()

            if result:
                logger.info("找到会话记录: %s (%d 条)", session_id, len(result))
                return result
            else:
                logger.warning("未找到会话: %s", session_id)
                return None

        except connector.Error as err:
            logger.error("加载会话失败 [%s]: %s", session_id, err)
            return None

        finally:
            if cursor:
                cursor.close()
            if conn and conn.is_connected():
                conn.close()

    @fastmcp.tool()
    def save_session(session_id: str, stage: Literal["guide","product"], content: list, status_code: int = 0, count: int = 1) -> dict:
        """
        将当前阶段的会话数据持久化到 sessions 表。

        Args:
            session_id: 会话唯一标识
            stage: 当前 Agent 阶段（guide/product）
            status_code: 会话状态码，与 stage 联合定位记录
            content: 对话消息列表，格式为 [{role, message, ...}, ...]
            count: 会话访问次数，默认 1

        Returns:
            dict，成功时包含 {"success": True, "session_id": "...", "message": "会话保存成功"}；
            失败时包含 {"success": False, "error": "错误描述"}
        """
        conn = None
        cursor = None

        try:
            conn = get_conn()
            cursor = conn.cursor()
            now = get_now_datetime_str()
            cursor.execute("""
                INSERT INTO sessions (session_id, stage, status_code, count, content, create_at, update_at)
                VALUES (%s, %s, %s, %s, %s, %s, %s)
                ON DUPLICATE KEY UPDATE
                    content=VALUES(content),
                    count=VALUES(count),
                    update_at=VALUES(update_at)
            """, (session_id, stage, status_code, count, content, now, now))
            conn.commit()

            logger.info("会话保存成功: session_id=%s stage=%s status_code=%d", session_id, stage, status_code)
            return {"success": True, "session_id": session_id, "stage": stage, "message": "会话保存成功"}

        except connector.Error as err:
            logger.error("保存会话失败 [%s]: %s", session_id, err)
            if conn:
                conn.rollback()
            return {"success": False, "error": str(err), "session_id": session_id}

        finally:
            if cursor:
                cursor.close()
            if conn and conn.is_connected():
                conn.close()

    @fastmcp.tool()
    def load_user_profile(user_id: str) -> Optional[str]:
        """
        从 user_summary 表加载用户的个人偏好摘要。

        Args:
            user_id: 用户唯一标识

        Returns:
            用户的偏好摘要 JSON 字符串（如品牌偏好、价格区间等）；
            未找到或出错时返回 None
        """
        conn = None
        cursor = None
        try:
            conn = get_conn()
            cursor = conn.cursor(dictionary=True)
            cursor.execute("SELECT * FROM user_summary WHERE user_id = %s", (user_id,))
            result = cursor.fetchone()

            if result:
                logger.info("找到用户记录: %s", user_id)
                return result["summary"]
            else:
                logger.warning("未找到用户: %s", user_id)
                return None

        except connector.Error as err:
            logger.error("加载用户画像失败 [%s]: %s", user_id, err)
            return None

        finally:
            if cursor:
                cursor.close()
            if conn and conn.is_connected():
                conn.close()

    @fastmcp.tool()
    def save_user_summary(user_id: str, summary: str) -> dict:
        """
        保存或更新用户的消费特征摘要到 user_summary 表。

        Args:
            user_id: 用户唯一标识
            summary: 用户画像摘要文本内容

        Returns:
            dict，成功时包含 {"success": True, "user_id": "..."}；
            失败时包含 {"success": False, "error": "错误描述"}
        """
        conn = None
        cursor = None
        try:
            conn = get_conn()
            cursor = conn.cursor()
            now = get_now_datetime_str()
            cursor.execute("""
                INSERT INTO user_summary (user_id, summary, create_at, update_at)
                VALUES (%s, %s, %s, %s)
                ON DUPLICATE KEY UPDATE
                    summary=VALUES(summary),
                    update_at=VALUES(update_at)
            """, (user_id, summary, now, now))
            conn.commit()
            logger.info("用户画像保存成功: user_id=%s", user_id)
            return {"success": True, "user_id": user_id, "message": "用户画像保存成功"}
        except connector.Error as err:
            logger.error("保存用户画像失败 [%s]: %s", user_id, err)
            if conn:
                conn.rollback()
            return {"success": False, "error": str(err), "user_id": user_id}
        finally:
            if cursor:
                cursor.close()
            if conn and conn.is_connected():
                conn.close()

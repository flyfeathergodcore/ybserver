"""
MCP 客户端 — 封装 SSE 连接、角色工具发现与工具调用。

基于 script/test_mcp_toollist.py 中的客户端逻辑提炼为可复用类。

用法:
    from mcp_servers.client import MCPClient

    client = MCPClient("http://localhost:8888/sse", role="guide_agent")
    client.connect()

    # 获取当前角色可见的工具列表
    tools = client.list_tools()
    for t in tools:
        print(t.name, t.description)

    # 调用工具
    result = client.call_tool("find_product_prompt", {"product": "手机"})

    # 获取工具描述文本（供 LLM prompt 注入）
    ctx = client.get_skill_context()

    client.close()
"""

import asyncio
import json
import logging
import threading
from typing import Any, Optional

from mcp.client.sse import sse_client
from mcp.client.session import ClientSession
from mcp.types import TextContent

logger = logging.getLogger(__name__)


class MCPClient:
    """MCP 客户端 — 封装 SSE 连接、工具发现与调用。

    内部维护一个后台事件循环线程，对外提供同步接口，
    方便 Agent 等同步代码调用。

    角色隔离通过 X-Agent-Role header 传递给服务端，
    服务端据此过滤可见工具列表和校验调用权限。
    """

    def __init__(self, server_url: str = "http://localhost:8888/sse", role: str = ""):
        """
        Args:
            server_url: MCP 服务端 SSE 地址
            role: 角色名称，如 "guide_agent" / "product_agent"；
                  通过 X-Agent-Role header 传递给服务端实现权限隔离
        """
        self.server_url = server_url
        self.role = role

        # 后台事件循环与线程
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None

        # SSE 连接与 MCP 会话
        self._sse_cm = None  # sse_client 的 async context manager
        self._session_cm = None  # ClientSession 的 async context manager
        self._session: Optional[ClientSession] = None

        # 工具列表缓存
        self._tools_cache: Optional[list] = None
        self._connected = False

    # ──────────────────────────────────────────────
    # 连接管理
    # ──────────────────────────────────────────────

    @classmethod
    def from_config(cls, config: dict):
        """从配置字典创建 MCPClient 实例"""
        server_url = config.get("mcp", {}).get("server_url", "http://localhost:8888/sse")
        role = config.get("mcp", {}).get("role", "")
        return cls(server_url=server_url, role=role)

    @property
    def _headers(self) -> dict:
        """构建携带角色信息的请求头"""
        if self.role:
            return {"X-Agent-Role": self.role}
        return {}

    def connect(self):
        """连接 MCP 服务器（同步阻塞直到连接完成）"""
        if self._connected:
            return

        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thread.start()

        fut = asyncio.run_coroutine_threadsafe(self._connect_async(), self._loop)
        fut.result()  # 阻塞等待连接完成
        self._connected = True
        logger.info("MCP 客户端已连接: server=%s role=%s", self.server_url, self.role)

    async def _connect_async(self):
        """异步连接 SSE，初始化 MCP 会话"""
        # 1. 进入 SSE 客户端上下文
        self._sse_cm = sse_client(self.server_url, headers=self._headers)
        self._read_stream, self._write_stream = await self._sse_cm.__aenter__()

        # 2. 进入 MCP 会话上下文
        self._session_cm = ClientSession(self._read_stream, self._write_stream)
        self._session = await self._session_cm.__aenter__()

        # 3. 初始化会话
        await self._session.initialize()

    def _run(self, coro) -> Any:
        """在后台事件循环上运行协程并等待结果"""
        if not self._loop or not self._connected:
            raise RuntimeError("MCPClient 未连接，请先调用 connect()")
        fut = asyncio.run_coroutine_threadsafe(coro, self._loop)
        return fut.result()

    def close(self):
        """断开 MCP 连接，清理资源"""
        if not self._connected:
            return

        try:
            # 在事件循环上执行关闭
            self._run(self._close_async())
        except Exception as e:
            logger.warning("MCP 关闭连接异常: %s", e)
        finally:
            # 停止事件循环
            if self._loop:
                self._loop.call_soon_threadsafe(self._loop.stop)
                if self._thread:
                    self._thread.join(timeout=5)
                self._loop.close()
                self._loop = None
                self._thread = None

            self._session = None
            self._tools_cache = None
            self._connected = False
            logger.info("MCP 客户端已断开")

    async def _close_async(self):
        """异步关闭 SSE 连接和 MCP 会话"""
        if self._session_cm:
            await self._session_cm.__aexit__(None, None, None)
            self._session_cm = None
        if self._sse_cm:
            await self._sse_cm.__aexit__(None, None, None)
            self._sse_cm = None

    # ──────────────────────────────────────────────
    # 工具发现
    # ──────────────────────────────────────────────

    def list_tools(self) -> list:
        """获取当前角色可见的工具列表（结果缓存，重复调用不重复请求）

        Returns:
            每个元素为 mcp.types.Tool 对象，包含:
              - name: 工具名
              - description: 工具描述
              - inputSchema: JSON Schema 格式的参数定义
        """
        if self._tools_cache is None:
            result = self._run(self._session.list_tools())
            self._tools_cache = result.tools
            logger.info("获取到 %d 个可见工具", len(self._tools_cache))
        return self._tools_cache

    def get_tool_names(self) -> list[str]:
        """获取当前角色可见的工具名称列表（轻量接口）"""
        return [t.name for t in self.list_tools()]

    # ──────────────────────────────────────────────
    # 工具调用
    # ──────────────────────────────────────────────

    def call_tool(self, tool_name: str, arguments: dict = None, **kwargs) -> Any:
        """调用 MCP 工具。

        Args:
            tool_name: 工具名称（需在当前角色可见列表中）
            arguments: 工具参数字典，与 kwargs 二选一
            **kwargs:  关键字参数形式

        Returns:
            JSON 解析后的结果（dict/list），纯文本时直接返回 str，
            无内容返回 None。
            工具调用返回错误时，返回 {"error": "错误描述", ...} 格式。
        """
        params = arguments if arguments is not None else kwargs
        if not isinstance(params, dict):
            raise ValueError(f"参数必须为 dict，收到: {type(params).__name__}")

        logger.info("工具调用: %s role=%s", tool_name, self.role)
        result = self._run(self._session.call_tool(tool_name, params))

        # 检查是否返回错误
        if hasattr(result, "isError") and result.isError:
            texts = self._extract_texts(result.content)
            error_msg = texts or f"工具 {tool_name} 调用返回错误"
            logger.warning("工具调用返回错误: %s → %s", tool_name, error_msg)
            return {"error": error_msg}

        # 从 TextContent 中提取文本
        text = self._extract_texts(result.content)
        if not text:
            return None

        logger.debug("工具调用成功: %s (%d 字符)", tool_name, len(text))

        # 尝试解析 JSON
        try:
            return json.loads(text)
        except (json.JSONDecodeError, TypeError):
            return text

    @staticmethod
    def _extract_texts(content_list: list) -> str:
        """从 MCP 返回的 content 列表中提取所有文本"""
        texts = []
        for c in content_list:
            if isinstance(c, TextContent):
                texts.append(c.text)
        return "\n".join(texts)

    # ──────────────────────────────────────────────
    # 技能上下文（供 LLM prompt 注入）
    # ──────────────────────────────────────────────

    def get_skill_context(self) -> str:
        """获取当前角色可见工具的文本描述。

        Agent 可将此结果注入 LLM prompt 的 {skills_context} 占位符，
        让 LLM 了解当前可用工具及其参数格式。

        Returns:
            格式化的工具描述文本，每项包含名称、描述和参数列表
        """
        tools = self.list_tools()
        lines = []
        for t in tools:
            desc = t.description or "无描述"
            lines.append(f"## {t.name}")
            lines.append(f"描述: {desc}")

            # 解析 inputSchema 生成参数列表
            schema = t.inputSchema or {}
            props = schema.get("properties", {})
            required = set(schema.get("required", []) or [])
            if props:
                lines.append("参数:")
                for pname, pinfo in props.items():
                    req_mark = "**必填**" if pname in required else "可选"
                    ptype = pinfo.get("type", "string")
                    pdesc = pinfo.get("description", "")
                    # 如果有枚举值，展示
                    enum_vals = pinfo.get("enum")
                    if enum_vals:
                        pdesc += f" 可选值: {', '.join(str(v) for v in enum_vals)}"
                    lines.append(f"  - {pname} ({ptype}, {req_mark}): {pdesc}")
            lines.append("")  # 空行分隔

        return "\n".join(lines)

    # ──────────────────────────────────────────────
    # 上下文管理器支持
    # ──────────────────────────────────────────────

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

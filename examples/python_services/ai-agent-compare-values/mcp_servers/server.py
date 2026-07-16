import logging
import importlib
import os
import pkgutil
import sys
import contextvars
from pathlib import Path
from .depend.log_config import setup_logging
from .depend.auth import set_role
from typing import Any

logger = logging.getLogger(__name__)

# 当前请求的角色（contextvars → 每个 asyncio 任务独立）
_current_role: contextvars.ContextVar[str | None] = contextvars.ContextVar('current_role', default=None)


# ============ 带权限的 FastMCP 子类 ============

from mcp.server.fastmcp import FastMCP as BaseFastMCP


class AuthFastMCP(BaseFastMCP):
    """
    带权限检查的 FastMCP。
    通过 Starlette 中间件 + contextvars 传递角色身份。
    """

    def sse_app(self, mount_path=None):
        """在 SSE app 上挂载纯 ASGI 中间件（不破坏 SSE 流）"""
        app = super().sse_app(mount_path)
        inner = app  # Starlette ASGI app

        # 纯 ASGI 包装层：不解析响应体，只读请求 header
        async def role_middleware(scope, receive, send):
            if scope["type"] == "http":
                headers = dict(scope.get("headers", []))
                role = None
                for k, v in headers.items():
                    if isinstance(k, bytes) and k.lower() == b"x-agent-role":
                        role = v.decode()
                        break
                _current_role.set(role)
            await inner(scope, receive, send)

        return role_middleware

    async def call_tool(self, name: str, arguments: dict[str, Any]):
        """调用工具前校验角色权限（在 FastMCP 层统一检查，不依赖装饰器 context 传播）"""
        role = _current_role.get()
        logger.info("工具调用: %s, 角色: %s", name, role)

        # 权限检查（在此层完成，避免 FastMCP 内部调度丢失 contextvars）
        from .depend.auth import list_protected_tools
        permissions = list_protected_tools()
        allowed_roles = permissions.get(name)
        if allowed_roles and (role is None or role not in allowed_roles):
            raise PermissionError(f"[权限拒绝] '{name}' 需要角色 {allowed_roles}，当前角色: {role}")
        return await super().call_tool(name, arguments)

    async def list_tools(self):
        """按角色过滤可见工具列表"""
        role = _current_role.get()
        from .depend.auth import list_protected_tools
        permissions = list_protected_tools()

        all_tools = await super().list_tools()
        filtered = []
        for t in all_tools:
            allowed_roles = permissions.get(t.name)
            if allowed_roles is None or (role and role in allowed_roles):
                filtered.append(t)
        logger.debug("工具列表: %s 可见 %d/%d 个", role or "无角色", len(filtered), len(all_tools))
        return filtered


# ============ MCP 服务器类 ============

class MCPServer:
    """基于 AuthFastMCP 的 MCP 服务器，支持工具权限隔离。"""

    def __init__(self, port: int = 8888, host: str = '0.0.0.0',
                 tool_path: str = None, config_path: str = None):
        self.server = AuthFastMCP(host=host, port=port)
        self.port = port
        self.host = host
        self.config_path = config_path
        if tool_path is None:
            self.tool_path = Path(__file__).parent / "skills"
        else:
            self.tool_path = Path(tool_path)
        self.running = False

    def find_tools(self) -> None:
        """自动发现工具模块并加载。"""
        if self.tool_path.is_file() and self.tool_path.suffix == '.py':
            module_name = self.tool_path.stem
            try:
                spec = importlib.util.spec_from_file_location(module_name, self.tool_path)
                if spec and spec.loader:
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                    logger.info("已加载工具模块: %s", module_name)
                    if hasattr(module, 'register_skill'):
                        module.register_skill(self.server)
                        logger.info("  已注册技能: %s", module_name)
            except Exception as e:
                logger.error("加载模块 %s 失败: %s", module_name, e)
            return

        if self.tool_path.is_dir():
            init_file = self.tool_path / "__init__.py"
            if init_file.exists():
                package_name = self.tool_path.name
                try:
                    package = importlib.import_module(package_name)
                    for module_info in pkgutil.iter_modules(package.__path__, package.__name__ + "."):
                        if module_info.name.endswith(".__init__"):
                            continue
                        try:
                            module = importlib.import_module(module_info.name)
                            logger.info("已加载工具模块: %s", module_info.name)
                            if hasattr(module, 'register_skill'):
                                module.register_skill(self.server)
                                logger.info("  已注册技能: %s", module_info.name)
                        except Exception as e:
                            logger.error("加载模块 %s 失败: %s", module_info.name, e)
                except ImportError as e:
                    logger.warning("无法导入包 '%s'，改用文件扫描: %s", package_name, e)
                    self._scan_directory()
            else:
                self._scan_directory()
            return
        logger.warning("工具路径无效: %s", self.tool_path)

    def _scan_directory(self):
        # 确保 mcp_servers/ 在 sys.path 中，技能模块才能 import depend.*
        mcp_dir = str(self.tool_path.parent)
        if mcp_dir not in sys.path:
            sys.path.insert(0, mcp_dir)

        for py_file in self.tool_path.glob("*.py"):
            if py_file.name == "__init__.py":
                continue
            module_name = py_file.stem
            try:
                spec = importlib.util.spec_from_file_location(module_name, py_file)
                if spec and spec.loader:
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                    logger.info("已加载工具模块: %s", module_name)
                    if hasattr(module, 'register_skill'):
                        module.register_skill(self.server)
                        logger.info("  已注册技能: %s", module_name)
            except Exception as e:
                logger.error("加载模块 %s 失败: %s", module_name, e)

    def start(self):
        self.running = True
        logger.info("正在启动 MCP 服务器...")
        self.find_tools()

        from .depend.auth import list_protected_tools
        protected = list_protected_tools()
        if protected:
            logger.info("工具权限配置: %s", protected)

        self.server.run(transport="sse")


# ============ Agent 初始化（替代已弃用的 mcp_setup） ============

def init_agents(config: dict[str, Any] | None = None):
    """
    初始化 Agent 运行所需的所有组件，并启动 MCP SSE 服务端。

    替代已弃用的 mcp_setup.create_mcp_services / create_session_factory。

    Returns:
        (llm, sessions, get_or_create_session)
    """
    import threading
    import time as _time
    from core.config import load_config
    from core.llm_client import LLMClient
    from .client import MCPClient
    from core.guide_agent import ShoppingGuideAgent
    from core.product_agent import ProductAgent

    cfg = config or load_config()
    llm_cfg = cfg.get("llm", {})
    # 环境变量覆盖 config.yaml，方便 Docker 部署时切换地址
    llm_base_url = os.environ.get("LLM_BASE_URL") or llm_cfg.get("base_url", "http://localhost:11434/v1")
    llm_model = os.environ.get("LLM_MODEL") or llm_cfg.get("model", "llama3:8b")
    llm = LLMClient(
        api_key=llm_cfg.get("api_key", "ollama"),
        base_url=llm_base_url,
        model=llm_model,
        timeout=llm_cfg.get("timeout", 60),
        temperature=llm_cfg.get("temperature", 0.3),
        max_tokens=llm_cfg.get("max_tokens", 2048),
    )

    # ── 启动 MCP SSE 服务端（后台线程）──
    mcp_cfg = cfg.get("mcp", {})
    mcp_port = mcp_cfg.get("port", 8765)
    _mcp_server = MCPServer(port=mcp_port, host="0.0.0.0")
    _t = threading.Thread(target=_mcp_server.start, daemon=True)
    _t.start()
    _time.sleep(1.5)  # 等待服务端加载工具并启动完成
    logger.info("MCP SSE 服务已启动: :%d", mcp_port)

    # MCP 客户端使用与服务端一致的端口
    _mcp_url = f"http://localhost:{mcp_port}/sse"

    sessions: dict[str, dict] = {}

    def get_or_create_session(sid: str) -> dict:
        if sid not in sessions:
            sessions[sid] = {
                "guide": ShoppingGuideAgent(llm=llm, config=cfg),
                "product": ProductAgent(llm=llm, mcp=MCPClient(server_url=_mcp_url, role="product_agent"), config=cfg),
                "stage": "guide",
                "product_history": [],
                "last_action": "",
            }
        return sessions[sid]

    return llm, sessions, get_or_create_session


# ============ 入口点 ============

if __name__ == "__main__":
    setup_logging()
    server = MCPServer(port=8888, host="0.0.0.0")
    server.start()

"""
权限隔离模块 — 装饰器 + 运行时检查。

用法:
    # 1. 在工具函数上加装饰器
    from depend.auth import require_role

    @fastmcp.tool()
    @require_role("product_agent")
    def search_category(...):
        ...

    # 2. 上层 (server.py) 在工具调用前设置角色
    from depend.auth import set_role
    set_role("product_agent")   # 从连接参数提取后设置
"""

import contextvars
from functools import wraps

_role_var: contextvars.ContextVar[str | None] = contextvars.ContextVar('auth_role', default=None)
_tool_permissions: dict[str, set[str]] = {}


def require_role(*roles: str):
    """
    装饰器：标记工具函数需要哪些角色才能调用。

    Args:
        *roles: 允许调用该工具的角色列表，如 "product_agent", "guide_agent", "admin"

    用法:
        @fastmcp.tool()
        @require_role("product_agent", "admin")
        def search_category(...):
            ...
    """
    def decorator(func):
        _tool_permissions[func.__name__] = set(roles)

        @wraps(func)
        def wrapper(*args, **kwargs):
            current = _get_role()
            allowed = _tool_permissions.get(func.__name__, set())
            if allowed and current not in allowed:
                raise PermissionError(
                    f"[权限拒绝] '{func.__name__}' 需要角色 {allowed}，当前角色: {current}"
                )
            return func(*args, **kwargs)
        return wrapper
    return decorator


def set_role(role: str | None) -> None:
    """设置当前请求的角色（使用 contextvars，跨 asyncio 任务安全）"""
    _role_var.set(role)


def _get_role() -> str | None:
    """获取当前请求的角色"""
    return _role_var.get()


def list_protected_tools() -> dict[str, list[str]]:
    """列出所有受保护的工具及其所需角色"""
    return {name: sorted(roles) for name, roles in _tool_permissions.items()}

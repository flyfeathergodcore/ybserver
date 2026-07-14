"""
导购 Agent 共享工具函数。

消除 guide_agent.py 和 product_agent.py 之间的重复代码。
"""

import os


def load_text_file(path: str, default: str = "", strict: bool = True) -> str:
    """
    从文件加载文本，strip() 后返回。

    Args:
        path: 文件路径
        default: 文件不存在或空时的返回值
        strict: True 时文件不存在抛 FileNotFoundError，False 时返回 default

    Returns:
        文件内容（strip 后）
    """
    if not path or not os.path.exists(path):
        if strict:
            raise FileNotFoundError(f"文件不存在: {path}")
        return default
    with open(path, "r", encoding="utf-8") as f:
        return f.read().strip()


def build_kwargs_with_user_id(user_id: str, kwargs: dict) -> dict:
    """
    如果 user_id 非空，向 kwargs 添加 user_id。
    消除 guide_agent 中多处重复的 user_id 条件判断。
    """
    if user_id:
        kwargs["user_id"] = user_id
    return kwargs

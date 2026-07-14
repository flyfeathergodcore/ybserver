"""
JSON 提取与修复工具。

统一各文件中的 json.loads + 正则回退模式。
"""

import re
import json


def _try_fix_json(raw: str) -> dict | None:
    """尝试修复常见的 JSON 错误"""
    if not raw:
        return None

    raw = re.sub(r'```json\s*|\s*```', '', raw).strip()

    fixes = [
        (r',\s*}', '}'),
        (r',\s*]', ']'),
        (r"'([^']*)'", r'"\1"'),
        (r'(?<!")\n(?!")', '\\n'),
        (r'(\{|\s+|,)(\w+)(:)', r'\1"\2"\3'),
        (r',\s*$', ''),
    ]

    for pattern, replacement in fixes:
        raw = re.sub(pattern, replacement, raw)

    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return None


def _extract_json_from_text(raw: str) -> dict | None:
    """从文本中提取并解析 JSON（多层回退）"""
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass

    fixed = _try_fix_json(raw)
    if fixed:
        return fixed

    json_pattern = r'\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}'
    for match in re.findall(json_pattern, raw, re.DOTALL):
        fixed = _try_fix_json(match)
        if fixed:
            return fixed

    return _construct_fallback_json(raw)


def _construct_fallback_json(raw: str) -> dict | None:
    """根据关键词构造兜底 JSON"""
    raw_lower = raw.lower()

    if 'tool' in raw_lower:
        tool_match = re.search(r'tool_name["\']?\s*[:=]\s*["\']?([^"\'}\s,]+)', raw)
        kwargs_match = re.search(r'kwargs["\']?\s*[:=]\s*["\']?([^"\'}\s,]+)', raw)
        if tool_match:
            return {
                "tool": True,
                "tool_name": tool_match.group(1),
                "kwargs": kwargs_match.group(1) if kwargs_match else "未知品类",
            }

    if 'question' in raw_lower:
        q_match = re.search(r'question_content["\']?\s*[:=]\s*["\']?([^"\'}\s,]+)', raw)
        return {"question": True, "question_content": q_match.group(1) if q_match else "请提供更多信息"}

    if 'final' in raw_lower:
        return {"final": True, "final_content": {}}

    if 'promote' in raw_lower:
        pm = re.search(r'promote_product["\']?\s*[:=]\s*(\{[^}]*\})', raw, re.DOTALL)
        if pm:
            try:
                return {"promote": True, "promote_product": json.loads(pm.group(1))}
            except json.JSONDecodeError:
                pass
        return {"promote": True, "promote_product": {"推荐产品": "产品理由"}}

    return None


def json_extract(text: str) -> dict | None:
    """
    统一 JSON 提取接口。从文本中提取 JSON 对象，多层回退。
    用于替代散落在各文件中的 json.loads + re.search 正则提取模式。
    """
    if not text:
        return None
    return _extract_json_from_text(text)

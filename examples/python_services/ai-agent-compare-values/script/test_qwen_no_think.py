"""
测试 Qwen3 抑制思考模式的各种参数组合

使用方法：
  python script/test_qwen_no_think.py

输出每个测试的完整响应，对比是否符合"直接回答不思考"的要求。
"""

import json, os, sys, time, urllib.request, urllib.error

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from core.llm_client import LLMClient


def test_prompt(label: str, extra_payload: dict | None = None, extra_system: str = ""):
    """单次测试：发送简单问题并返回完整响应"""
    endpoint = os.getenv("LLM_BASE_URL", "http://localhost:11434/api/chat")
    model = os.getenv("LLM_MODEL", "qwen3.5:9b")
    api_key = os.getenv("LLM_API_KEY", "ollama")
    endpoint = endpoint.rstrip("/") + "/chat/completions"

    messages = [
        {"role": "system", "content": f"你是一个有用的AI助手，请用中文简洁回答。{extra_system}".strip()},
        {"role": "user", "content": "1+1等于几？直接告诉我答案就行。"},
    ]

    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0,
        "max_tokens": 512,
        "stream": True,
        "think": False
    }
    if extra_payload:
        payload.update(extra_payload)

    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        endpoint,
        data=data,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
    )

    t0 = time.time()
    try:
        resp = urllib.request.urlopen(req, timeout=30)
        body = json.loads(resp.read())
        elapsed = (time.time() - t0) * 1000
        raw_content = body["choices"][0]["message"]["content"]
        # 提取 thinking 部分（Ollama 的 Qwen 可能返回 思考过程在内容开头）
        has_think = "思考" in raw_content[:200] or "嗯" in raw_content[:200] or "首先" in raw_content[:200] or "让我" in raw_content[:200]
        if not has_think and len(raw_content.split("\n")) > 3:
            has_think = True  # 多段输出也视为思考

        return {
            "label": label,
            "ok": not has_think,
            "elapsed_ms": f"{elapsed:.0f}ms",
            "content_len": len(raw_content),
            "content_preview": raw_content[:200].replace("\n", "↵"),
            "full": raw_content,
        }
    except Exception as e:
        return {"label": label, "ok": False, "elapsed_ms": "✗", "content_len": 0, "error": str(e), "content_preview": str(e)}


def main():
    print("=" * 60)
    print("Qwen3 抑制思考模式测试")
    print("=" * 60)

    tests = [
        # 1. 默认（对照）
        ("默认（无额外参数）", None, ""),
        # 2. 系统提示抑制
        ("system prompt 抑制", None, "不要输出推理过程，直接给出最终答案。"),
        # 3. raw=True
        ("raw=true", {"raw": True}, ""),
        # 4. raw=True + system prompt 抑制
        ("raw=true + 系统抑制", {"raw": True}, "不要输出推理过程，直接给出最终答案。"),
        # 5. max_thinking_tokens=0
        ("max_thinking_tokens=0", {"max_thinking_tokens": 0}, ""),
        # 6. reasoning_effort=none
        ("reasoning_effort=none", {"reasoning_effort": "none"}, ""),
        # 7. temperature=0 + seed=42（确定性输出）
        ("temperature=0 + seed=42", {"temperature": 0, "seed": 42}, ""),
        # 8. 组合：raw + system 抑制 + max_thinking_tokens=0
        ("raw + 系统抑制 + max_thinking_tokens=0",
         {"raw": True, "max_thinking_tokens": 0},
         "不要输出推理过程，直接给出最终答案。"),
    ]

    results = []
    for label, extra, system in tests:
        print(f"\n{'─' * 50}")
        print(f"▶ {label}")
        print(f"{'─' * 50}")
        r = test_prompt(label, extra, system)
        results.append(r)

        if r.get("error"):
            print(f"  错误: {r['error']}")
        else:
            print(f"  耗时: {r['elapsed_ms']}  |  长度: {r['content_len']}B")
            print(f"  状态: {'✅ 直接回答' if r['ok'] else '❌ 含思考'}")
            print(f"  预览: {r['content_preview'][:150]}")

    # ── 汇总表 ──
    print(f"\n\n{'=' * 60}")
    print("结果汇总")
    print(f"{'=' * 60}")
    print(f"{'配置':<35} {'耗时':>8} {'长度':>6} {'状态':>10}")
    print(f"{'─' * 61}")
    for r in results:
        status = "✅ 直接" if r.get("ok") else "❌ 思考"
        elapsed = r.get("elapsed_ms", "✗")
        length = str(r.get("content_len", 0))
        print(f"{r['label']:<35} {elapsed:>8} {length:>6} {status:>10}")

    # ── 全量响应 ──
    print(f"\n\n{'=' * 60}")
    print("完整响应原文")
    print(f"{'=' * 60}")
    for r in results:
        print(f"\n{'─' * 50}")
        print(f"# {r['label']}")
        print(f"{'─' * 50}")
        if r.get("full"):
            print(r["full"])
        elif r.get("error"):
            print(f"[错误] {r['error']}")


if __name__ == "__main__":
    main()

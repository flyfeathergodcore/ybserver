"""
LLM 客户端 — OpenAI 兼容 API

支持 Ollama / SiliconFlow / OpenAI 等兼容接口。
"""

import json
import os
import re
import time
import urllib.request
import urllib.error
from typing import Optional
from tools.json_parse import json_extract


class LLMClient:
    """
    轻量 LLM 客户端，兼容 OpenAI / SiliconFlow / Ollama 等 API。

    使用示例：
        llm = LLMClient(
            api_key="ollama",
            base_url="http://localhost:11434/v1",
            model="llama3:8b",
        )
        reply = llm.chat("你好")
    """

    def __init__(
        self,
        api_key: str,
        base_url: str = "https://api.siliconflow.cn/v1",
        model: str = "deepseek-ai/DeepSeek-V3",
        timeout: int = 60,
        temperature: float = 0.3,
        max_tokens: int = 2048,
    ):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout
        self.temperature = temperature
        self.max_tokens = max_tokens
        self._endpoint = f"{self.base_url}/chat/completions"

    def chat(self,
        user_message: str,
        system_prompt: str = "",
        history: Optional[list[dict]] = None,
    ) -> str:
        """发送对话请求，返回模型回复文本"""
        messages = []
        if system_prompt:
            messages.append({"role": "system", "content": system_prompt})
        if history:
            messages.extend(history)
        messages.append({"role": "user", "content": user_message})

        return self._request(messages)

    def chat_with_json(
        self,
        user_message: str,
        system_prompt: str = "",
        temperature: Optional[float] = None,
    ) -> dict:
        """
        发送请求并尝试返回 JSON。
        适用于需要结构化输出的场景。
        """
        messages = []
        if system_prompt:
            messages.append({"role": "system", "content": system_prompt})
        messages.append({"role": "user", "content": user_message})

        # 临时覆盖 temperature
        orig_temp = self.temperature
        if temperature is not None:
            self.temperature = temperature
        try:
            response = self._request(messages, response_format="json_object")
            result = json_extract(response)
            return result if result else {"raw": response}
        except json.JSONDecodeError:
            return {"raw": response}
        finally:
            self.temperature = orig_temp

    def chat_stream(self, user_message: str, system_prompt: str = ""):
        """
        流式对话请求，逐 token 产出。

        使用方式:
            for chunk in llm.chat_stream("你好"):
                print(chunk, end="", flush=True)
        """
        messages = []
        if system_prompt:
            messages.append({"role": "system", "content": system_prompt})
        messages.append({"role": "user", "content": user_message})

        payload = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
            "stream": True,
        }
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            self._endpoint,
            data=data,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
        )

        # ── 日志：Ollama 流式请求 ──
        total_chars = sum(len(m.get("content","")) for m in messages)
        last_msg_preview = messages[-1]["content"][:120].replace("\n"," ") if messages else ""
        sys_prompt_preview = messages[0]["content"][:80].replace("\n"," ") + "..." if len(messages) > 1 and messages[0]["role"] == "system" else ""
        print(f"[ollama] → {self.model} (stream)  msgs={len(messages)}  total={total_chars}B  temp={self.temperature}"
              f"{'  system:'+sys_prompt_preview if sys_prompt_preview else ''}"
              f"  last:{last_msg_preview}", flush=True)
        t0 = time.time()

        resp = urllib.request.urlopen(req, timeout=self.timeout)
        buf = b""
        token_count = 0
        while True:
            chunk = resp.read(1)
            if not chunk:
                break
            buf += chunk
            if b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line or line.startswith(b":"):
                    continue
                # SSE format: data: {...}
                if line.startswith(b"data: "):
                    line = line[6:]
                if line.strip() == b"[DONE]":
                    break
                try:
                    j = json.loads(line)
                    delta = j.get("choices", [{}])[0].get("delta", {})
                    content = delta.get("content", "")
                    if content:
                        token_count += 1
                        if token_count % 50 == 0:
                            preview = content[:30].replace("\n", " ")
                            print(f"[ollama] ... {token_count} tokens  last={preview}", flush=True)
                        yield content
                except json.JSONDecodeError:
                    continue

        t1 = time.time()
        print(f"[ollama] ← stream done  {token_count} tokens  {((t1-t0)*1000):.0f}ms", flush=True)

    def _request(
        self,
        messages: list[dict],
        response_format: str = "text",
    ) -> str:
        """发送 API 请求"""
        payload = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
        }
        if response_format == "json_object":
            payload["response_format"] = {"type": "json_object"}

        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            self._endpoint,
            data=data,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
        )

        # ── 日志：Ollama 请求（全部）──
        print(f"[ollama] → {self.model}  payload:\n{json.dumps(payload, ensure_ascii=False, indent=2)}", flush=True)
        t0 = time.time()

        try:
            resp = urllib.request.urlopen(req, timeout=self.timeout)
            body = json.loads(resp.read())
            t1 = time.time()
            content = body["choices"][0]["message"]["content"]
            preview = content[:120].replace("\n", " ")
            print(f"[ollama] ← {len(content)}B  {((t1-t0)*1000):.0f}ms  {preview}...", flush=True)
            return content
        except urllib.error.HTTPError as e:
            t1 = time.time()
            print(f"[ollama] ✗ HTTP {e.code}  {((t1-t0)*1000):.0f}ms", flush=True)
            raise RuntimeError(f"LLM API 请求失败: {e.code}") from e
        except urllib.error.URLError as e:
            t1 = time.time()
            print(f"[ollama] ✗ 连接失败  {((t1-t0)*1000):.0f}ms  {e.reason}", flush=True)
            raise RuntimeError(f"LLM API 连接失败: {e.reason}") from e

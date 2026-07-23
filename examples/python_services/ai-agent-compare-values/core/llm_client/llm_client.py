import os
import re
import json
import time
import logging
import httpx
import asyncio
from typing import Dict, List, Any, Optional, Union
from dataclasses import dataclass, asdict
from enum import Enum
from abc import ABC, abstractmethod

# ── LLM 请求/响应完整日志 ──
_llm_log = logging.getLogger("llm")
_llm_log.setLevel(logging.DEBUG)
_log_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "logs")
os.makedirs(_log_dir, exist_ok=True)
_log_file = os.path.join(_log_dir, "llm_calls.log")
_fh = logging.FileHandler(_log_file, encoding="utf-8")
_fh.setFormatter(logging.Formatter('%(message)s'))
_llm_log.addHandler(_fh)
_seq = [0]

def _llm_trace(messages, schema, result, elapsed, error=None):
    """记录完整 LLM 请求/响应到日志文件"""
    _seq[0] += 1
    entry = {
        "seq": _seq[0],
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "elapsed_ms": elapsed,
        "request": {
            "messages": [{"role": m.role.value, "content": m.content} for m in messages],
            "schema": schema,
        },
    }
    if error:
        entry["error"] = str(error)
    else:
        entry["response"] = json.loads(json.dumps(result, ensure_ascii=False, default=str))
    _llm_log.info(json.dumps(entry, ensure_ascii=False))

class ModelProvider(Enum):
    OLLAMA = "ollama"
    OPENAI = "openai"

class MessageType(Enum):
    USER = "user"
    ASSISTANT = "assistant"
    SYSTEM = "system"

@dataclass
class Message:
    """消息格式定义"""
    role: MessageType
    content: str
    name: Optional[str] = None
    
    def to_dict(self):
        return {
            "role": self.role.value,
            "content": self.content,
            **({"name": self.name} if self.name else {})
        }

class LLMConfig:
    """LLM配置类"""
    def __init__(self, provider: ModelProvider, api_key: str = None, 
                 base_url: str = None, model: str = None, **kwargs):
        self.provider = provider
        self.api_key = api_key
        self.base_url = base_url or self._get_default_base_url()
        self.model = model
        self.kwargs = kwargs
        
    def _get_default_base_url(self):
        """获取默认的API基础URL"""
        if self.provider == ModelProvider.OLLAMA:
            return "http://localhost:11434"
        elif self.provider == ModelProvider.OPENAI:
            return "https://api.openai.com/v1"
        return "http://localhost:11434"

class BaseModelClient(ABC):
    """基础模型客户端抽象类"""
    
    def __init__(self, config: LLMConfig):
        self.config = config
        self.client = httpx.AsyncClient(timeout=300.0)
    
    @abstractmethod
    async def generate(self, messages: List[Message], 
                      schema: Optional[Union[Dict, str]] = None, **kwargs) -> Dict[str, Any]:
        """生成响应"""
        pass
    
    @abstractmethod
    async def stream_generate(self, messages: List[Message], 
                             schema: Optional[Union[Dict, str]] = None, **kwargs):
        """流式生成响应"""
        pass
    
    async def close(self):
        """关闭客户端"""
        await self.client.aclose()

class OllamaClient(BaseModelClient):
    """Ollama客户端"""
    
    def __init__(self, config: LLMConfig):
        super().__init__(config)
        
    async def generate(self, messages: List[Message], 
                      schema: Optional[Union[Dict, str]] = None, **kwargs) -> Dict[str, Any]:
        """生成响应"""
        payload = {
            "model": self.config.model,
            "messages": [msg.to_dict() for msg in messages],
            "stream": False,
            **kwargs
        }
        
        # 如果有schema，添加格式支持
        if schema:
            if isinstance(schema, dict):
                payload["format"] = "json"
            elif isinstance(schema, str):
                payload["format"] = "json"
        
        response = await self.client.post(
            f"{self.config.base_url}/api/generate",
            json=payload,
            headers={
                "Content-Type": "application/json"
            }
        )
        
        if response.status_code != 200:
            raise Exception(f"Ollama API error: {response.text}")
            
        return response.json()
    
    async def stream_generate(self, messages: List[Message], 
                             schema: Optional[Union[Dict, str]] = None, **kwargs):
        """流式生成响应"""
        payload = {
            "model": self.config.model,
            "messages": [msg.to_dict() for msg in messages],
            "stream": True,
            **kwargs
        }
        
        if schema:
            if isinstance(schema, dict):
                payload["format"] = "json"
            elif isinstance(schema, str):
                payload["format"] = "json"
            
        async for response in self.client.aiter_lines(
            f"{self.config.base_url}/api/generate",
            json=payload,
            headers={
                "Content-Type": "application/json"
            }
        ):
            if response:
                try:
                    yield json.loads(response)
                except json.JSONDecodeError:
                    # 如果不是JSON，可能是文本内容
                    yield {"message": response}

class OpenAIClient(BaseModelClient):
    """OpenAI客户端"""
    
    def __init__(self, config: LLMConfig):
        super().__init__(config)
        
    async def generate(self, messages: List[Message], 
                      schema: Optional[Union[Dict, str]] = None, **kwargs) -> Dict[str, Any]:
        """生成响应"""
        payload = {
            "model": self.config.model,
            "messages": [msg.to_dict() for msg in messages],
            "stream": False,
            **kwargs
        }
        
        # 如果有schema，添加函数调用支持
        if schema:
            if isinstance(schema, dict):
                # 将dict转换为OpenAI工具格式
                payload["tools"] = [{
                    "type": "function",
                    "function": {
                        "name": "response_schema",
                        "description": "Response schema",
                        "parameters": schema
                    }
                }]
                # 强制模型必须使用 function calling（兼容 LM Studio 等只接受字符串的服务器）
                payload["tool_choice"] = "required"
            elif isinstance(schema, str):
                # 使用字符串名称
                payload["tools"] = [{
                    "type": "function",
                    "function": {
                        "name": schema,
                        "description": "Response schema",
                        "parameters": {
                            "type": "object",
                            "properties": {},
                            "required": []
                        }
                    }
                }]
                payload["tool_choice"] = "required"
        
        response = await self.client.post(
            f"{self.config.base_url}/chat/completions",
            json=payload,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.config.api_key}"
            }
        )
        
        if response.status_code != 200:
            raise Exception(f"OpenAI API error: {response.text}")
            
        return response.json()
    
    async def stream_generate(self, messages: List[Message], 
                             schema: Optional[Union[Dict, str]] = None, **kwargs):
        """流式生成响应"""
        payload = {
            "model": self.config.model,
            "messages": [msg.to_dict() for msg in messages],
            "stream": True,
            **kwargs
        }
        
        # 如果有schema，添加工具支持
        if schema:
            if isinstance(schema, dict):
                payload["tools"] = [{
                    "type": "function",
                    "function": {
                        "name": "response_schema",
                        "description": "Response schema",
                        "parameters": schema
                    }
                }]
            elif isinstance(schema, str):
                payload["tools"] = [{
                    "type": "function",
                    "function": {
                        "name": schema,
                        "description": "Response schema",
                        "parameters": {
                            "type": "object",
                            "properties": {},
                            "required": []
                        }
                    }
                }]
            
        async for response in self.client.aiter_lines(
            f"{self.config.base_url}/chat/completions",
            json=payload,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.config.api_key}"
            }
        ):
            if response.startswith("data:"):
                data = response[5:].strip()
                if data and data != "[DONE]":
                    try:
                        yield json.loads(data)
                    except json.JSONDecodeError:
                        continue

class LLMClient:
    """通用LLM客户端"""
    
    def __init__(self, provider: ModelProvider, api_key: str = None, 
                 base_url: str = None, model: str = None, **kwargs):
        self.config = LLMConfig(provider, api_key, base_url, model, **kwargs)
        
        # 根据提供商选择对应的客户端
        if provider == ModelProvider.OLLAMA:
            self.client = OllamaClient(self.config)
        elif provider == ModelProvider.OPENAI:
            self.client = OpenAIClient(self.config)
        else:
            raise ValueError(f"Unsupported provider: {provider}")
    
    async def generate(self, messages: List[Message],
                      schema: Optional[Union[Dict, str]] = None,
                      concurrency: int = 1, **kwargs) -> Dict[str, Any]:
        """生成响应

        当 schema 为 dict 时，并发发送 3 次请求，谁先返回且能被正确解析
        （包含 schema.required 中所有字段）就用谁，同时取消其余请求。
        3 次都失败则返回第一次完成的原始响应。
        """
        t0 = int(time.time() * 1000)
        if schema is None:
            result = await self.client.generate(messages, schema, **kwargs)
            _llm_trace(messages, None, result, int(time.time() * 1000) - t0)
            return result

        required_fields = schema.get("required", []) if isinstance(schema, dict) else []

        # 并发发送请求（默认 concurrency=1），as_completed 谁先返回先检查
        tasks = [
            asyncio.ensure_future(self.client.generate(messages, schema, **kwargs))
            for _ in range(concurrency)
        ]
        first_raw = None
        all_errors = []

        try:
            for coro in asyncio.as_completed(tasks):
                try:
                    raw = await coro
                except Exception as e:
                    all_errors.append(str(e))
                    print(f"[LLMClient] 一次请求异常: {e}")
                    continue

                if first_raw is None:
                    first_raw = raw

                try:
                    parsed = self._parase_schema(raw)
                    if isinstance(parsed, dict) and all(
                        parsed.get(field) is not None for field in required_fields
                    ):
                        for t in tasks:
                            t.cancel()
                        _llm_trace(messages, schema, parsed, int(time.time() * 1000) - t0)
                        return parsed
                except Exception:
                    pass
        finally:
            for t in tasks:
                t.cancel()

        elapsed = int(time.time() * 1000) - t0
        if first_raw is None:
            err = f"{concurrency} 次并发请求全部异常: {'; '.join(all_errors)}"
            _llm_trace(messages, schema, None, elapsed, error=err)
            raise Exception(err)
        print(f"[LLMClient] ⚠️ {concurrency} 次请求均未满足 required={required_fields}，返回原始响应")
        _llm_trace(messages, schema, first_raw, elapsed)
        return first_raw
    
    async def stream_generate(self, messages: List[Message], 
                             schema: Optional[Union[Dict, str]] = None, **kwargs):
        """流式生成响应"""
        async for chunk in self.client.stream_generate(messages, schema, **kwargs):
            yield chunk
    
    async def close(self):
        """关闭客户端"""
        await self.client.close()
    
    # 工具方法
    def create_message(self, role: MessageType, content: str, name: str = None) -> Message:
        """创建消息对象"""
        return Message(role, content, name)
    
    @classmethod
    def from_config(cls, config: Dict[str, Any]):
        """从配置创建客户端"""
        provider = ModelProvider(config.get("provider"))
        api_key = config.get("api_key")
        base_url = config.get("base_url")
        model = config.get("model")
        return cls(provider, api_key, base_url, model, **config.get("kwargs", {}))
    
    def _parase_schema(self, response: Dict[str, Any]) -> Dict:
        """解析schema为字典"""
        """从 OpenAI 响应中提取结构化数据"""
        try:
            choice = response["choices"][0]
            message = choice.get("message", {})

            if message.get("tool_calls"):
                # OpenAI 标准 function calling 格式 → 从 tool_calls 中提取 JSON
                tool_call = message["tool_calls"][0]
                arguments = tool_call["function"]["arguments"]
                return json.loads(arguments)

            # 部分本地模型（vLLM/Qwen 等）不支持 tool_calls，
            # 而是直接在 message.content 中返回 JSON 字符串
            content = message.get("content", "")
            if content:
                # 尝试 1: 直接解析 JSON
                try:
                    return json.loads(content)
                except (json.JSONDecodeError, TypeError):
                    pass

                # 尝试 2: 从 markdown 代码块中提取 JSON（如 ```json ... ```）
                md_match = re.search(r'```(?:json)?\s*\n?([\s\S]*?)```', content)
                if md_match:
                    try:
                        return json.loads(md_match.group(1).strip())
                    except (json.JSONDecodeError, TypeError):
                        pass

                # 尝试 3: 正则提取第一个 JSON 对象（处理模型在 JSON 前后附加文字的情况）
                json_match = re.search(r'\{[\s\S]*\}', content)
                if json_match:
                    try:
                        return json.loads(json_match.group(0))
                    except (json.JSONDecodeError, TypeError):
                        pass

                # 尝试 4: 解析 XML-like 标签格式
                # <parameter=name>value</parameter> → {"name": "value"}
                param_matches = re.findall(
                    r'<parameter=(\w+)>\s*([\s\S]*?)\s*</parameter>',
                    content
                )
                if param_matches:
                    result = {}
                    for name, value in param_matches:
                        value = value.strip()
                        # 尝试转换布尔值
                        if value.lower() in ("true", "false"):
                            value = value.lower() == "true"
                        # 尝试转换数字
                        elif value.replace(".", "").replace("-", "").isdigit():
                            try:
                                value = float(value) if "." in value else int(value)
                            except ValueError:
                                pass
                        result[name] = value
                    if result:
                        return result

                # 调试：如果所有解析方式都失败，打印原始内容
                print(f"[DEBUG _parase_schema] 无法解析 content，前 500 字符: {content[:500]}")

            # 兜底：返回原始响应
            return response
        except (KeyError, IndexError, json.JSONDecodeError) as e:
            raise ValueError(f"Failed to extract structured data: {e}")

# 创建客户端的便捷方法
def create_ollama_client(model: str, base_url: str = "http://localhost:11434", 
                       api_key: str = None, **kwargs) -> LLMClient:
    """创建Ollama客户端"""
    return LLMClient(
        provider=ModelProvider.OLLAMA,
        api_key=api_key,
        base_url=base_url,
        model=model,
        **kwargs
    )

def create_openai_client(model: str, api_key: str, base_url: str = None, **kwargs) -> LLMClient:
    """创建OpenAI客户端"""
    return LLMClient(
        provider=ModelProvider.OPENAI,
        api_key=api_key,
        base_url=base_url,
        model=model,
        **kwargs
    )

async def example_usage():
    """使用示例"""
    
    # 创建OpenAI客户端 - 支持传入API密钥
    config = {
        "provider": ModelProvider.OPENAI,
        "api_key": "518678",
        "base_url": "http://127.0.0.1:8000/v1",
        "model": "Qwen3.6-27B-MLX-4bit",
        "kwargs": {
            "temperature": 0.7,
            "max_tokens": 1000
        }
    }
    client = LLMClient.from_config(config)
    
    # 打印配置信息用于调试
    print(f"🔧 Config:")
    print(f"  Provider: {client.config.provider}")
    print(f"  Base URL: {client.config.base_url}")
    print(f"  Model: {client.config.model}")
    print(f"  API Key: {client.config.api_key[:4]}***")
    
    # 定义schema字典
    schema = {
        "type": "object",
        "properties": {
            "agent_analyse": {"type": "string"},
            "reply": {"type": "string"},
            "intent_delta": {
                "core_need": {"type": "string"},
                "constraints": {"type": "array", "items": {"type": "string"}},
                "budget": {
                    "current": {"type": "number"},
                    "min": {"type": "number"},
                    "max": {"type": "number"},
                    "confidence": {"type": "string"},
                    "state": {"type": "bool"}
                },
                "status": {"type": "string"}
            }
        },
        "required": ["agent_analyse", "reply", "intent_delta"]
    }
    
    # 创建消息
    messages = [
        client.create_message(MessageType.SYSTEM, "You are a helpful assistant"),
        client.create_message(MessageType.USER, "Process this request and return structured data")
    ]
    
    try:
        print("🚀 Sending request...")
        result = await client.generate(messages, schema)
        print("✅ Generated result:", json.dumps(result, indent=2))
        
    except httpx.ConnectError as e:
        print(f"❌ Connection Error: {e}")
        print("   请确认服务是否运行在 http://127.0.0.1:8000")
        print("   可以用: curl http://127.0.0.1:8000/v1/models 测试")
    except httpx.HTTPStatusError as e:
        print(f"❌ HTTP Status Error: {e.response.status_code}")
        print(f"   Response: {e.response.text}")
    except Exception as e:
        print(f"❌ Error: {e}")
        print(f"   Error Type: {type(e).__name__}")
        import traceback
        print("   Full traceback:")
        traceback.print_exc()
    finally:
        await client.close()

def load_config_from_file(config_path: str):
    """从配置文件加载配置"""
    with open(config_path, 'r') as f:
        config = json.load(f)
    return config

# 配置文件示例内容 (config.json)
config_example = {
    "provider": "openai",
    "api_key": "518678",
    "base_url": "https://127.0.0.1:8000/v1",
    "model": "Qwen3.5-9B-OptiQ-4bit",
    "kwargs": {
        "temperature": 0.7,
        "max_tokens": 1000
    }
}

if __name__ == "__main__":
    # 运行示例
    asyncio.run(example_usage())

import httpx
from typing import Optional, Dict, Any, Iterator, Callable
import json
import re
import time
from functools import wraps

class OllamaClient:
    """
    Ollama API 客户端，支持流式和非流式输出
    
    使用示例:
        # 非流式
        client = OllamaClient(model="qwen3.5:9b")
        reply = client.chat("你好")
        
        # 流式
        for chunk in client.chat_stream("你好"):
            print(chunk, end="", flush=True)
    """
    
    def __init__(
        self,
        base_url: str = "http://localhost:11434",
        model: str = "qwen3.5:9b-mlx",
        timeout: int = 60,
        temperature: float = 0.3,
        max_tokens: int = 2048,
        max_retries: int = 3,
    ):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.max_retries = max_retries
        self.client = httpx.Client(timeout=timeout)
        
        # 统计信息
        self.stats = {
            "total_requests": 0,
            "successful_requests": 0,
            "failed_requests": 0,
            "total_tokens": 0,
        }
    
    @classmethod
    def from_config(cls, config: Dict[str, Any]) -> "OllamaClient":
        """从配置字典创建 OllamaClient 实例"""
        return cls(
            base_url=config.get("base_url", "http://localhost:11434"),
            model=config.get("model", "qwen3.5:9b-mlx"),
            timeout=config.get("timeout", 60),
            temperature=config.get("temperature", 0.3),
            max_tokens=config.get("max_tokens", 2048),
            max_retries=config.get("max_retries", 3),
        )
    # ==================== 公共接口 ====================
    
    def chat(self, user_message: str, system_prompt: str = "") -> str:
        """非流式普通对话"""
        prompt = self._build_prompt(user_message, system_prompt)
        payload = self._build_payload(prompt)
        
        self.stats["total_requests"] += 1
        
        try:
            response = self._make_request(payload)
            content = response.get("response", "")
            self.stats["successful_requests"] += 1
            return content
        except Exception as e:
            self.stats["failed_requests"] += 1
            raise
    
    def chat_stream(
        self, 
        user_message: str, 
        system_prompt: str = "",
        callback: Optional[Callable[[str], None]] = None,
        show_progress: bool = True,
    ) -> Iterator[str]:
        """
        流式对话，逐 token 产出
        
        Args:
            user_message: 用户消息
            system_prompt: 系统提示
            callback: 可选的回调函数，每收到一个 token 时调用
            show_progress: 是否显示进度信息
        
        Yields:
            str: 每个 token 的内容
            
        使用示例:
            for chunk in client.chat_stream("你好"):
                print(chunk, end="", flush=True)
        """
        prompt = self._build_prompt(user_message, system_prompt)
        
        payload = {
            "model": self.model,
            "prompt": prompt,
            "stream": True,
            "options": {
                "temperature": self.temperature,
                "num_predict": self.max_tokens,
            }
        }
        
        if show_progress:
            print(f"[Ollama] → 流式请求: {self.model}")
            print(f"[Ollama] → Prompt 长度: {len(prompt)} 字符")
        
        self.stats["total_requests"] += 1
        
        try:
            with self.client.stream(
                "POST",
                f"{self.base_url}/api/generate",
                json=payload,
            ) as response:
                response.raise_for_status()
                
                token_count = 0
                full_response = ""
                start_time = time.time()
                
                for line in response.iter_lines():
                    if not line:
                        continue
                    
                    try:
                        data = json.loads(line)
                        token = data.get("response", "")
                        
                        if token:
                            token_count += 1
                            full_response += token
                            
                            # 调用回调函数
                            if callback:
                                callback(token)
                            
                            # 每 50 个 token 打印进度
                            if show_progress and token_count % 50 == 0:
                                preview = token[:30].replace("\n", " ")
                                print(f"[Ollama] ... {token_count} tokens  last={preview}", flush=True)
                            
                            yield token
                        
                        # 检查是否完成
                        if data.get("done", False):
                            break
                            
                    except json.JSONDecodeError:
                        continue
                
                elapsed = time.time() - start_time
                self.stats["successful_requests"] += 1
                self.stats["total_tokens"] += token_count
                
                if show_progress:
                    print(f"[Ollama] ← 流式完成  {token_count} tokens  {elapsed*1000:.0f}ms")
                
        except Exception as e:
            self.stats["failed_requests"] += 1
            if show_progress:
                print(f"[Ollama] ✗ 流式请求失败: {e}")
            raise
    
    def chat_json(
        self, 
        user_message: str, 
        system_prompt: str = "",
        schema: Optional[Dict] = None,
        validate: bool = True,
    ) -> Dict[str, Any]:
        """
        非流式 JSON 输出
        
        Args:
            user_message: 用户消息
            system_prompt: 系统提示
            schema: 可选的 JSON Schema 验证
            validate: 是否验证输出是否符合 Schema
        """
        json_system = self._build_json_system_prompt(system_prompt, schema)
        user_prompt = f"{user_message}\n\n请只输出有效的 JSON 格式，不要包含任何其他文字。"
        prompt = self._build_prompt(user_prompt, json_system)
        
        payload = self._build_payload(prompt, temperature=0.1)
        
        self.stats["total_requests"] += 1
        
        print(f"[Ollama] → JSON 请求: {self.model}")
        print(f"[Ollama] → Prompt 长度: {len(prompt)} 字符")
        
        try:
            response = self._make_request(payload)
            raw_content = response.get("response", "")
            
            print(f"[Ollama] ← 响应长度: {len(raw_content)} 字符")
            if raw_content:
                print(f"[Ollama] ← 原始响应预览: {raw_content[:200]}...")
            
            result = self._extract_json(raw_content)
            
            if validate and schema and not self._validate_json_schema(result, schema):
                print("[Ollama] ⚠️ JSON 不符合 Schema")
                return {"error": "Schema 验证失败", "data": result}
            
            self.stats["successful_requests"] += 1
            return result
                
        except Exception as e:
            self.stats["failed_requests"] += 1
            print(f"[Ollama] ✗ JSON 请求失败: {e}")
            return {"error": str(e), "raw": raw_content if 'raw_content' in locals() else ""}
    
    def chat_json_stream(
        self,
        user_message: str,
        system_prompt: str = "",
        schema: Optional[Dict] = None,
        callback: Optional[Callable[[Dict], None]] = None,
        show_progress: bool = True,
    ) -> Iterator[Dict]:
        """
        流式 JSON 输出，边生成边解析
        
        Args:
            user_message: 用户消息
            system_prompt: 系统提示
            schema: 可选的 JSON Schema
            callback: 回调函数，每次解析到有效 JSON 时调用
            show_progress: 是否显示进度信息
        
        Yields:
            dict: 累积的 JSON 对象
        """
        json_system = self._build_json_system_prompt(system_prompt, schema)
        user_prompt = f"{user_message}\n\n请只输出有效的 JSON 格式，不要包含任何其他文字。"
        prompt = self._build_prompt(user_prompt, json_system)
        
        payload = {
            "model": self.model,
            "prompt": prompt,
            "stream": True,
            "options": {
                "temperature": 0.1,
                "num_predict": self.max_tokens,
            }
        }
        
        if show_progress:
            print(f"[Ollama] → JSON 流式请求: {self.model}")
            print(f"[Ollama] → Prompt 长度: {len(prompt)} 字符")
        
        self.stats["total_requests"] += 1
        
        accumulated = ""
        token_count = 0
        start_time = time.time()
        last_yielded = None
        
        try:
            with self.client.stream(
                "POST",
                f"{self.base_url}/api/generate",
                json=payload,
            ) as response:
                response.raise_for_status()
                
                for line in response.iter_lines():
                    if not line:
                        continue
                    
                    try:
                        data = json.loads(line)
                        token = data.get("response", "")
                        
                        if token:
                            token_count += 1
                            accumulated += token
                            
                            # 尝试解析当前累积的 JSON
                            try:
                                parsed = self._extract_json(accumulated)
                                if "error" not in parsed and parsed != last_yielded:
                                    last_yielded = parsed
                                    if callback:
                                        callback(parsed)
                                    yield parsed
                            except:
                                pass
                            
                            if show_progress and token_count % 50 == 0:
                                preview = token[:30].replace("\n", " ")
                                print(f"[Ollama] ... {token_count} tokens  last={preview}", flush=True)
                        
                        if data.get("done", False):
                            # 最后尝试完整解析
                            final_result = self._extract_json(accumulated)
                            if "error" not in final_result and final_result != last_yielded:
                                yield final_result
                            break
                            
                    except json.JSONDecodeError:
                        continue
                
                elapsed = time.time() - start_time
                self.stats["successful_requests"] += 1
                self.stats["total_tokens"] += token_count
                
                if show_progress:
                    print(f"[Ollama] ← JSON 流式完成  {token_count} tokens  {elapsed*1000:.0f}ms")
                
        except Exception as e:
            self.stats["failed_requests"] += 1
            if show_progress:
                print(f"[Ollama] ✗ JSON 流式请求失败: {e}")
            raise
    
    # ==================== 内部方法 ====================
    
    def _make_request(self, payload: dict) -> dict:
        """发送请求，支持重试"""
        for attempt in range(self.max_retries):
            try:
                response = self.client.post(
                    f"{self.base_url}/api/generate",
                    json=payload,
                )
                response.raise_for_status()
                return response.json()
            except httpx.HTTPStatusError as e:
                if e.response.status_code == 429:
                    wait_time = (attempt + 1) * 2
                    print(f"[Ollama] ⚠️ 请求过多，等待 {wait_time} 秒...")
                    time.sleep(wait_time)
                    continue
                raise
            except Exception as e:
                if attempt == self.max_retries - 1:
                    raise
                wait_time = (attempt + 1) * 1
                print(f"[Ollama] ⚠️ 请求失败，{wait_time} 秒后重试: {e}")
                time.sleep(wait_time)
        
        raise RuntimeError("请求失败，已达到最大重试次数")
    
    def _build_payload(self, prompt: str, temperature: Optional[float] = None) -> dict:
        """构建请求载荷"""
        return {
            "model": self.model,
            "prompt": prompt,
            "stream": False,
            "options": {
                "temperature": temperature if temperature is not None else self.temperature,
                "num_predict": self.max_tokens,
            }
        }
    
    def _build_json_system_prompt(self, system_prompt: str, schema: Optional[Dict] = None) -> str:
        """构建 JSON 输出的系统提示"""
        json_instruction = "你是一个 JSON 输出助手。你的所有回复必须是有效的 JSON 格式，不要包含任何额外的解释、标记或格式化文本。"
        
        if schema:
            schema_str = json.dumps(schema, ensure_ascii=False, indent=2)
            json_instruction += f"\n\n输出的 JSON 必须符合以下 Schema:\n{schema_str}"
        
        if system_prompt:
            return f"{system_prompt}\n\n{json_instruction}"
        return json_instruction
    
    def _build_prompt(self, user_message: str, system_prompt: str = "") -> str:
        """构建提示词"""
        prompt = ""
        if system_prompt:
            prompt += f"System: {system_prompt}\n\n"
        prompt += f"User: {user_message}\n\nAssistant: "
        return prompt
    
    def _extract_json(self, raw_content: str) -> dict:
        """从响应中提取 JSON"""
        if not raw_content:
            return {"error": "空响应", "raw": ""}
        
        methods = [
            self._parse_direct,
            self._parse_braces,
            self._parse_brackets,
            self._parse_regex,
        ]
        
        for method in methods:
            result = method(raw_content)
            if result and "error" not in result:
                return result
        
        return {"raw": raw_content, "error": "无法解析为 JSON"}
    
    def _parse_direct(self, content: str) -> Optional[dict]:
        try:
            return json.loads(content)
        except:
            return None
    
    def _parse_braces(self, content: str) -> Optional[dict]:
        brace_count = 0
        start_idx = -1
        for i, char in enumerate(content):
            if char == '{':
                if brace_count == 0:
                    start_idx = i
                brace_count += 1
            elif char == '}':
                brace_count -= 1
                if brace_count == 0 and start_idx != -1:
                    try:
                        return json.loads(content[start_idx:i+1])
                    except:
                        continue
        return None
    
    def _parse_brackets(self, content: str) -> Optional[dict]:
        bracket_count = 0
        start_idx = -1
        for i, char in enumerate(content):
            if char == '[':
                if bracket_count == 0:
                    start_idx = i
                bracket_count += 1
            elif char == ']':
                bracket_count -= 1
                if bracket_count == 0 and start_idx != -1:
                    try:
                        return json.loads(content[start_idx:i+1])
                    except:
                        continue
        return None
    
    def _parse_regex(self, content: str) -> Optional[dict]:
        patterns = [
            r'\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}',
            r'\[[^\[\]]*(?:\[[^\[\]]*\][^\[\]]*)*\]',
        ]
        
        for pattern in patterns:
            matches = re.findall(pattern, content, re.DOTALL)
            for match in matches:
                try:
                    return json.loads(match)
                except:
                    continue
        return None
    
    def _validate_json_schema(self, data: dict, schema: dict) -> bool:
        """递归验证 JSON 是否符合 Schema（支持嵌套对象）"""
        try:
            return self._validate_schema_recursive(data, schema)
        except Exception as e:
            print(f"[Ollama] ⚠️ Schema 验证失败: {e}")
            return False

    def _validate_schema_recursive(self, data: any, schema: dict) -> bool:
        """递归验证的核心实现"""
        
        # 1. 验证数据类型
        expected_type = schema.get("type")
        if expected_type:
            if expected_type == "object":
                if not isinstance(data, dict):
                    print(f"[Ollama] ⚠️ 期望 object，实际得到 {type(data).__name__}")
                    return False
                return self._validate_object(data, schema)
            elif expected_type == "array":
                if not isinstance(data, list):
                    print(f"[Ollama] ⚠️ 期望 array，实际得到 {type(data).__name__}")
                    return False
                return self._validate_array(data, schema)
            elif expected_type == "string":
                return isinstance(data, str)
            elif expected_type == "integer":
                return isinstance(data, int)
            elif expected_type == "number":
                return isinstance(data, (int, float))
            elif expected_type == "boolean":
                return isinstance(data, bool)
            elif expected_type == "null":
                return data is None
        
        # 如果没有 type 字段，默认验证通过
        return True

    def _validate_object(self, data: dict, schema: dict) -> bool:
        """验证 object 类型"""
        
        # 1. 检查必需字段
        required = schema.get("required", [])
        for field in required:
            if field not in data:
                print(f"[Ollama] ⚠️ 缺少必需字段: {field}")
                return False
        
        # 2. 检查 properties
        properties = schema.get("properties", {})
        additional_props = schema.get("additionalProperties", True)
        
        for key, value in data.items():
            if key in properties:
                # 递归验证嵌套字段
                if not self._validate_schema_recursive(value, properties[key]):
                    print(f"[Ollama] ⚠️ 字段 '{key}' 验证失败")
                    return False
            elif not additional_props:
                print(f"[Ollama] ⚠️ 不允许的额外字段: {key}")
                return False
        
        return True

    def _validate_array(self, data: list, schema: dict) -> bool:
        """验证 array 类型"""
        
        # 检查数组项类型
        items_schema = schema.get("items")
        if items_schema:
            for idx, item in enumerate(data):
                if not self._validate_schema_recursive(item, items_schema):
                    print(f"[Ollama] ⚠️ 数组第 {idx} 项验证失败")
                    return False
        
        # 检查数组长度限制
        min_items = schema.get("minItems")
        if min_items is not None and len(data) < min_items:
            print(f"[Ollama] ⚠️ 数组长度 {len(data)} 小于最小值 {min_items}")
            return False
        
        max_items = schema.get("maxItems")
        if max_items is not None and len(data) > max_items:
            print(f"[Ollama] ⚠️ 数组长度 {len(data)} 大于最大值 {max_items}")
            return False
        
        return True
    
    def get_stats(self) -> dict:
        """获取统计信息"""
        return self.stats

    def chat_json_concurrent(
        self, 
        user_message: str, 
        system_prompt: str = "",
        schema: Optional[Dict] = None,
        validate: bool = True,
        max_concurrent: int = 3,
    ) -> Dict[str, Any]:
        """
        并发请求，直到获得有效的 Schema 响应
        
        Args:
            user_message: 用户消息
            system_prompt: 系统提示
            schema: 可选的 JSON Schema 验证
            validate: 是否验证输出是否符合 Schema
            max_concurrent: 最大并发请求数
            
        Returns:
            第一个成功验证的响应结果
        """
        import concurrent.futures
        
        # 创建多个变体客户端（不同温度等参数变化）
        attempts = []
        for i in range(max_concurrent):
            # 使用不同的温度参数提高成功概率
            temp = self.temperature + (i * 0.1) 
            client = OllamaClient(
                base_url=self.base_url,
                model=self.model,
                temperature=temp,
                max_tokens=self.max_tokens,
                max_retries=self.max_retries,
            )
            attempts.append(client)
        
        results = {}
        
        def make_request(client, idx):
            try:
                result = client.chat_json(user_message, system_prompt, schema, validate)
                return idx, result, None
            except Exception as e:
                return idx, None, str(e)
        
        # 并发执行请求
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_concurrent) as executor:
            futures = {executor.submit(make_request, client, i): i 
                      for i, client in enumerate(attempts)}
            
            for future in concurrent.futures.as_completed(futures):
                idx, result, error = future.result()
                results[idx] = (result, error)
        
        # 返回第一个成功的响应（不带错误）
        for idx in range(max_concurrent):
            result, error = results.get(idx, (None, None))
            if result is not None and "error" not in result:
                return result
        
        # 如果所有都失败了，返回第一个结果
        if results:
            first_idx = list(results.keys())[0]
            return results[first_idx][0] if results[first_idx][0] is not None else {"error": "所有请求都失败了"}

        return {"error": "所有请求都失败了"}




if __name__ == "__main__":
    def demo_streaming():
        """演示流式输出功能"""
        print("=" * 60)
        print("🚀 Ollama 流式输出演示")
        print("=" * 60)
        
        client = OllamaClient(
            model="qwen3.5:9b-mlx",
            temperature=0.7,
            max_tokens=2048,
        )
        
        # 示例 1: 基础流式对话
        print("\n📝 示例 1: 基础流式对话")
        print("🤖: ", end="", flush=True)
        
        full_text = ""
        for chunk in client.chat_stream("请用一句话介绍什么是人工智能"):
            print(chunk, end="", flush=True)
            full_text += chunk
            # 模拟打字效果（可选）
            # time.sleep(0.01)
        print("\n")
        
        # 示例 2: 带回调的流式
        print("\n📝 示例 2: 带回调的流式")
        
        token_count = 0
        def on_token(token: str):
            nonlocal token_count
            token_count += 1
            # 可以在这里做实时处理，比如更新 UI
            pass
        
        print("🤖: ", end="", flush=True)
        for chunk in client.chat_stream(
            "请用一句话介绍机器学习",
            callback=on_token
        ):
            print(chunk, end="", flush=True)
        print(f"\n\n📊 共收到 {token_count} 个 token")
        
        # 示例 3: JSON 流式输出
        print("\n" + "=" * 60)
        print("📊 示例 3: JSON 流式输出")
        
        print("生成中...")
        last_json = None
        
        for partial_json in client.chat_json_stream(
            "生成一个包含 name, age, city, hobbies 的 JSON 描述一个人"
        ):
            last_json = partial_json
            # 实时显示进度
            print(f"\r当前解析: {json.dumps(partial_json, ensure_ascii=False)}", end="", flush=True)
        
        print("\n\n最终 JSON:")
        print(json.dumps(last_json, ensure_ascii=False, indent=2))
        
        # 示例 4: 带 Schema 的 JSON 流式
        print("\n" + "=" * 60)
        print("📊 示例 4: 带 Schema 的 JSON 流式")
        
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
                        "state":{"type": "bool"}
                    },
                    "status": {"type": "string"}
                }
            },
            "required": ["agent_analyse", "reply", "intent_delta"]
        }
        
        print("生成中...")
        for partial_json in client.chat_json_stream(
            "生成一个符合指定 Schema 的 JSON，描述购物意图",
            schema=schema
        ):
            pass  # 只收集最终结果
        
        print("\n最终 JSON（带 Schema）:")
        print(json.dumps(partial_json, ensure_ascii=False, indent=2))
        
        # 显示统计
        print("\n" + "=" * 60)
        print("📊 统计信息:")
        print(json.dumps(client.get_stats(), ensure_ascii=False, indent=2))
    
        # 添加并发测试示例
        print("\n" + "=" * 60)
        print("🚀 并发测试示例")
        print("=" * 60)
        
        # 创建一个简单的schema用于测试
        test_schema = {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "age": {"type": "integer"},
                "city": {"type": "string"}
            },
            "required": ["name", "age"]
        }
        
        # 使用并发方法测试
        print("\n🧪 测试并发JSON输出:")
        concurrent_result = client.chat_json_concurrent(
            "生成一个包含姓名、年龄、城市的JSON对象",
            schema=test_schema,
            max_concurrent=3
        )
        
        print(f"并发结果: {json.dumps(concurrent_result, ensure_ascii=False, indent=2)}")


    # 添加并发测试示例
        print("\n" + "=" * 60)
        print("🚀 并发测试示例")
        print("=" * 60)
        
        # 创建一个简单的schema用于测试
        test_schema = {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "age": {"type": "integer"},
                "city": {"type": "string"}
            },
            "required": ["name", "age"]
        }
        
        # 使用并发方法测试
        print("\n🧪 测试并发JSON输出:")
        concurrent_result = client.chat_json_concurrent(
            "生成一个包含姓名、年龄、城市的JSON对象",
            schema=test_schema,
            max_concurrent=3
        )
        
        print(f"并发结果: {json.dumps(concurrent_result, ensure_ascii=False, indent=2)}")


demo_streaming()
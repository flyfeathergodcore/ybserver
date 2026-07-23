from typing import Dict, List, Any, Optional, Union
import json
import os
import asyncio
from core.llm_client.llm_client import LLMClient, ModelProvider, MessageType, Message
from mcp_servers.client import MCPClient
from core.product_agent.product_statemachine import ProductStateMachine


class ProductAgent:
    def __init__(self, config: dict):
        self.config = config
        self._llm = LLMClient.from_config(config)
        self._mcp = MCPClient.from_config(config)
        self._mcp.connect()  # 先连接再获取工具列表
        self._tool_list = self._mcp.list_tools()
        self.state_machine = ProductStateMachine()

        # 提示词目录
        self._prompts_dir = os.path.join(os.path.dirname(__file__), '..', '..', 'prompts')

        # 短期记忆
        self._user_id:str = None
        self._session_id:str = None
        self.current_schema = None
        self.conversation_history = []
        self._intent: str = ""
        self.short_memory = []
        self.max_retries = 3
        self.user_profile = "无用户画像信息"
        self.planning_str = ""
        self._search_results = []  # 存储搜索阶段返回的原始产品数据

    # 加载schema以及初始化self._intent
    def set_schema(self, schema: Dict[str, Any]):
        self.current_schema = schema
        self._intent = self._intent_fix()

    def reset(self):
        """重置 ProductAgent 状态（供 gRPC ResetSession 调用）"""
        self.current_schema = None
        self._intent = ""
        self.conversation_history = []
        self.short_memory = []
        self.planning_str = ""
        self._search_results = []
        try:
            self.state_machine.fail()
            self.state_machine.reset()
        except Exception:
            pass

    def set_user_session(self, user_id: str, session_id: str):
        self._user_id = user_id
        self._session_id = session_id
        self._load_user_profile({"user_id": user_id, "session_id": session_id})

    # 加载提示词模板
    def _load_prompt(self, filename: str) -> str:
        """从文件加载提示词模板"""
        filepath = os.path.join(self._prompts_dir, filename)
        if not os.path.exists(filepath):
            return ""
        with open(filepath, 'r', encoding='utf-8') as f:
            return f.read()

    # 转化用户需求
    def _intent_fix(self)->str:
        """
        转换用户需求为文字描述，便于后续的 LLM 分析和规划
        """
        if self.current_schema is None:
            raise ValueError("当前 schema 未设置，请先调用 set_schema()")
        
        response = "以下是汇总后，用户购物需求：\n"
        if "category" in self.current_schema:
            response += f"产品类型: {self.current_schema['category']}\n"
        if "core_need" in self.current_schema:
            response += f"核心需求: {self.current_schema['core_need']}\n"
        if "parameters" in self.current_schema and isinstance(self.current_schema["parameters"], list):
            response += f"参数需求: {self.current_schema['parameters']}\n"
        if "constraints" in self.current_schema and isinstance(self.current_schema["constraints"], list):
            self.current_schema["constraints"] = [str(c) for c in self.current_schema["constraints"]]
            response += f"约束条件: {', '.join(self.current_schema['constraints'])}\n"
        if "preferences" in self.current_schema and isinstance(self.current_schema["preferences"], list):
            response += f"偏好倾向: {', '.join(str(p) for p in self.current_schema['preferences'])}\n"
        # 兼容嵌套 budget 和扁平 budget_min/budget_max
        if "budget" in self.current_schema and isinstance(self.current_schema["budget"], dict):
            b = self.current_schema["budget"]
            response += f"预算信息: {b.get('min', '?')} - {b.get('max', '?')} 元\n"
        elif "budget_min" in self.current_schema or "budget_max" in self.current_schema:
            bmin = self.current_schema.get("budget_min", "?")
            bmax = self.current_schema.get("budget_max", "?")
            response += f"预算信息: {bmin} - {bmax} 元\n"
        return response

    # 加载用户画像
    def _load_user_profile(self, user_profile: Dict[str, Any]) -> bool:
        if not self._user_id:
            print("[ProductAgent] 用户ID未设置，跳过加载用户画像")
            self.user_profile = "无用户画像信息"
            return False
        try:
            self._mcp.connect()
            self.user_profile = self._mcp.call_tool("load_user_profile", {"user_id": self._user_id})
            return True
        except Exception as e:
            print(f"[ProductAgent] Error occurred while fetching user profile: {e}")
            self.user_profile = "加载用户画像失败"
        return False

    # 转换历史对话为str
    def _format_conversation_history(self) -> str:
        """将历史对话转换为字符串，便于 LLM 分析"""
        history_str = ""
        for entry in self.conversation_history[-10:]:  # 只取最近10轮对话
            role = entry.get("role", "user")
            content = entry.get("content", "")
            history_str += f"{role}: {content}\n"
        return history_str.strip()
    
    def _format_planning(self, response: Dict[str, Any]) -> str:
        """将规划结果格式化为字符串，便于 LLM 分析"""
        planning_str = "规划结果:\n"
        for key, value in response.items():
            planning_str += f"{key}: {value}\n"
        self.planning_str = planning_str.strip()
        return planning_str.strip()
    
    # 准备运行时
    def runtime_prepare(self, prompt_dict: dict):
        # 根据传入的 key 确定要加载哪个 prompt 文件（不再依赖状态机状态）
        prompt_file = (
            prompt_dict.get("planning_prompt_file")
            or prompt_dict.get("promote_prompt_file")
            or prompt_dict.get("detail_prompt_file")
            or prompt_dict.get("done_prompt_file")
            or ""
        )
        prompt_template = self._load_prompt(prompt_file)


        self._load_user_profile({"user_id": self._user_id, "session_id": self._session_id})
        if not self.user_profile:
            self.user_profile = "无用户画像信息"

        # 工具列表转为可读字符串
        tools_str = ""
        for t in self._tool_list:
            name = getattr(t, 'name', str(t))
            desc = getattr(t, 'description', '') or ''
            tools_str += f"- {name}: {desc}\n"

        prompt_template = prompt_template.replace("{planning_result}", self.planning_str or "")
        prompt_template = prompt_template.replace("{user_profile}", str(self.user_profile))
        prompt_template = prompt_template.replace("{history}", self._format_conversation_history())
        prompt_template = prompt_template.replace("{short_memory}", self._format_short_memory() if hasattr(self, '_format_short_memory') else "")
        prompt_template = prompt_template.replace("{intent}", self._intent or "")
        prompt_template = prompt_template.replace("{tools_list}", tools_str)

        return prompt_template

    def _build_tools_desc(self) -> str:
        """构建工具列表描述文本，供 LLM prompt 使用（含参数详情、必填标记）"""
        lines = []
        for t in self._tool_list:
            name = getattr(t, 'name', str(t))
            desc = (getattr(t, 'description', '') or '').strip()
            schema = getattr(t, 'inputSchema', {}) or {}
            props = schema.get('properties', {})
            req_set = set(schema.get('required', []) or [])

            lines.append(f"### {name}")
            lines.append(f"描述: {desc}")
            if props:
                lines.append("参数:")
                for pname, pinfo in props.items():
                    ptype = pinfo.get('type', 'string')
                    pdesc = pinfo.get('description', '')
                    req_mark = "【必填】" if pname in req_set else "可选"
                    lines.append(f"  {req_mark} {pname} ({ptype}): {pdesc}")
            lines.append("")  # 空行分隔
        return '\n'.join(lines)

    async def planning_step(self, retry_count: int = 0) -> Dict[str, Any]:
        max_retries_per_stage = self.max_retries

        # ==================== 阶段1: 制定搜索计划 ====================
        # intent 已由 GuideAgent 确认，无需再反问用户
        stage1_retry = 0
        while stage1_retry < max_retries_per_stage:
            try:
                messages = [
                    self._llm.create_message(MessageType.SYSTEM, (
                        "你是一个资深购物顾问。用户的购物需求已经确认，不要反问用户任何问题。"
                        "你的任务是：根据已确认的需求和可用工具，制定一个具体的搜索计划。"
                        "搜索策略：1) 优先按价位筛选（如有预算）；2) 按品牌查同品牌商品（如有品牌偏好）；"
                        "3) 用关键词搜索具体名称/品牌/型号，避免抽象词如'旗舰''高端'。"
                    )),
                    self._llm.create_message(MessageType.USER, self.runtime_prepare({
                        "planning_prompt_file": "product_planning.txt"
                    }))
                ]
                schema = {
                    "type": "object",
                    "properties": {
                        "steps": {
                            "type": "array",
                            "items": {"type": "string"},
                            "description": "按顺序列出的搜索步骤，每步说明用哪个工具、什么参数"
                        }
                    },
                    "required": ["steps"]
                }
                result = await self._llm.generate(messages, schema=schema)
            except Exception as e:
                print(f"[ProductAgent] LLM 规划阶段出错: {e}")
                stage1_retry += 1
                if stage1_retry >= max_retries_per_stage:
                    return {"status": "error", "message": f"LLM 规划阶段失败: {str(e)}"}
                continue

            self._format_planning(result)
            break

        # ==================== 阶段2: 执行搜索（ReAct 循环，真正调用 MCP） ====================
        self._search_results = []  # 存为实例变量，供 consulting 追问细节时使用
        stage2_retry = 0
        tool_call_log = []       # 记录每次调用的工具名和参数
        max_iterations = 10
        max_empty_tool = 3       # 连续未指定工具名的最大次数
        iteration = 0
        empty_tool_count = 0

        while iteration < max_iterations:
            iteration += 1
            try:
                # 构建上下文：已执行的工具调用历史 + 结果
                executed_context = ""
                if tool_call_log:
                    executed_context = "## 已执行的工具调用及结果\n"
                    for i, (log, res) in enumerate(zip(tool_call_log, self._search_results), 1):
                        executed_context += f"{i}. {log['tool_name']}({json.dumps(log['tool_arguments'], ensure_ascii=False)})\n"
                        res_str = json.dumps(res, ensure_ascii=False, default=str)
                        executed_context += f"   结果: {res_str[:800]}\n\n"

                messages = [
                    self._llm.create_message(MessageType.SYSTEM, (
                        "你是一个资深购物顾问。请按照搜索计划逐步执行搜索。"
                        "每一步决定：调用哪个工具、传什么参数。如果计划已执行完或已有足够信息，设 promote=true。"
                        "不要反问用户，直接决定。"
                    )),
                    self._llm.create_message(MessageType.USER, (
                        f"## 已确认的用户需求\n{self._intent or ''}\n\n"
                        f"## 搜索计划\n{self.planning_str}\n\n"
                        f"{executed_context}"
                        f"## 可用工具\n{self._build_tools_desc()}\n\n"
                        "请决定下一步：调用哪个工具？如果搜索已够，promote 设为 true。"
                    ))
                ]
                schema = {
                    "type": "object",
                    "properties": {
                        "tool_calls": {"type": "boolean", "description": "是否还需要调用工具"},
                        "tool_name": {"type": "string", "description": "要调用的工具名称"},
                        "tool_arguments": {"type": "object", "description": "传给工具的参数"},
                        "promote": {"type": "boolean", "description": "搜索计划是否已全部执行完成"},
                    },
                    "required": ["tool_calls", "promote"]
                }
                result = await self._llm.generate(messages, schema=schema)
            except Exception as e:
                print(f"[ProductAgent] LLM 执行搜索阶段出错 (iter={iteration}): {e}")
                stage2_retry += 1
                if stage2_retry >= max_retries_per_stage:
                    return {"status": "error", "message": f"LLM 执行搜索阶段失败: {str(e)}"}
                continue

            promote = result.get("promote", False)
            # 兼容两种 LLM 返回格式：布尔值 或 数组
            raw_tool_calls = result.get("tool_calls", False)
            if isinstance(raw_tool_calls, list) and len(raw_tool_calls) > 0:
                # 格式: {"tool_calls": [{"tool_name": "...", "tool_arguments": {...}}]}
                first_call = raw_tool_calls[0]
                tool_calls = True
                tool_name = first_call.get("tool_name", "") or first_call.get("name", "")
                tool_args = first_call.get("tool_arguments", {}) or first_call.get("parameters", {}) or first_call.get("tool_args", {}) or first_call.get("arguments", {}) or {}
            elif isinstance(raw_tool_calls, bool):
                tool_calls = raw_tool_calls
                tool_name = result.get("tool_name", "")
                tool_args = result.get("tool_arguments", {}) or {}
            else:
                tool_calls = bool(raw_tool_calls)
                tool_name = result.get("tool_name", "")
                tool_args = result.get("tool_arguments", {}) or {}

            if promote or not tool_calls:
                print(f"[ProductAgent] 阶段2 完成: promote={promote}, tool_calls={tool_calls}, iterations={iteration}")
                break

            # 如果 LLM 给的工具名或参数为空，跳过（计入空计数，连续N次退出）
            if not tool_name or not tool_args:
                empty_tool_count += 1
                print(f"[ProductAgent] 阶段2 工具名/参数为空 ({empty_tool_count}/{max_empty_tool}): name={tool_name!r} args={tool_args!r}")
                tool_call_log.append({"tool_name": "(empty)", "tool_arguments": {}})
                if empty_tool_count >= max_empty_tool:
                    print(f"[ProductAgent] 阶段2 连续{max_empty_tool}次未指定工具名，退出搜索")
                    break
                continue
            empty_tool_count = 0

            # 真正调用 MCP 工具
            print(f"[ProductAgent] 阶段2 调用工具: {tool_name}({json.dumps(tool_args, ensure_ascii=False)})")

            try:
                mcp_result = self._mcp.call_tool(tool_name, tool_args)
                if mcp_result is None:
                    mcp_result = {"info": "工具返回空（可能API未配置或无结果）"}
                self._search_results.append(mcp_result)
                tool_call_log.append({"tool_name": tool_name, "tool_arguments": tool_args})
                print(f"[ProductAgent] 阶段2 工具结果: {str(mcp_result)[:200]}")
            except Exception as e:
                print(f"[ProductAgent] 阶段2 工具调用失败: {tool_name} → {e}")
                self._search_results.append({"error": str(e)})
                tool_call_log.append({"tool_name": tool_name, "tool_arguments": tool_args})

        if iteration >= max_iterations:
            print(f"[ProductAgent] 阶段2 达到最大迭代次数 {max_iterations}")

        # PLANNING 结束，返回搜索结果（不输出推荐）
        return {"status": "searched", "data": self._search_results}

    async def _promote_step(self) -> Dict[str, Any]:
        """PROMOTE 阶段：比对搜索产品和用户需求，输出推荐"""
        max_retries_per_stage = self.max_retries
        self.state_machine.promote()  # PLANNING → PROMOTE

        # ==================== 阶段3: 推荐产品 ====================
        stage3_retry = 0
        while stage3_retry < max_retries_per_stage:
            try:
                # 格式化搜索结果
                search_context = ""
                if self._search_results:
                    search_context = "## 搜索结果\n"
                    for i, res in enumerate(self._search_results, 1):
                        search_context += f"{i}. {json.dumps(res, ensure_ascii=False, default=str)[:1000]}\n\n"

                messages = [
                    self._llm.create_message(MessageType.SYSTEM, (
                        "你是一个资深购物顾问。请根据真实的搜索结果，筛选最匹配用户需求的产品并推荐。"
                        "每个推荐必须基于搜索结果中的真实数据（产品名称、价格等）。"
                        "不要编造产品ID，使用搜索结果中实际存在的产品标识。"
                    )),
                    self._llm.create_message(MessageType.USER, (
                        f"## 已确认的用户需求\n{self._intent or ''}\n\n"
                        f"## 搜索计划\n{self.planning_str}\n\n"
                        f"{search_context}"
                        "请基于以上真实搜索结果，推荐最合适的产品。"
                    ))
                ]
                schema = {
                    "type": "object",
                    "properties": {
                        "recommendations": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "product_id": {"type": "string", "description": "产品ID"},
                                    "reason": {"type": "string", "description": "推荐理由"}
                                },
                                "required": ["product_id", "reason"]
                            }
                        }
                    },
                    "required": ["recommendations"]
                }
                result = await self._llm.generate(messages, schema=schema)
            except Exception as e:
                print(f"[ProductAgent] LLM 推荐阶段出错: {e}")
                stage3_retry += 1
                if stage3_retry >= max_retries_per_stage:
                    return {"status": "error", "message": f"LLM 推荐阶段失败: {str(e)}"}
                continue

            if result.get("recommendations"):
                return {"status": "success", "data": result}
            else:
                stage3_retry += 1
                if stage3_retry >= max_retries_per_stage:
                    return {"status": "error", "message": "LLM 推荐阶段未返回有效结果"}
                continue


    async def _select_top_products(self, products: List[Dict], intent: Dict,
                                    user_query: str, search_plan: Dict) -> List[Dict]:
        """
        使用 LLM 分析候选商品，按用户需求和约束筛选出最优的 10 个产品。

        这是 plan-action 模式中的 "plan" 阶段：
        - 将搜索到的所有候选商品交给 LLM
        - LLM 根据用户意图、预算、约束条件进行综合评估
        - 返回评分最高的前 10 个产品

        Args:
            products: 搜索返回的候选商品列表（可能 20-30 个）
            intent: 用户意图（category, core_need, constraints, budget）
            user_query: 用户原始输入
            search_plan: 搜索计划

        Returns:
            经 LLM 筛选后的前 10 个产品列表
        """
        # 构建筛选提示词
        prompt = self._build_select_prompt(products, intent, user_query, search_plan)

        # 定义输出 schema
        select_schema = {
            "type": "object",
            "properties": {
                "selection_reason": {
                    "type": "string",
                    "description": "筛选理由，说明为什么选择这些产品"
                },
                "selected_products": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "index": {"type": "integer", "description": "在原列表中的索引"},
                            "product_id": {"type": "string", "description": "产品ID"},
                            "score": {"type": "number", "description": "综合评分（0-100）"},
                            "match_reason": {"type": "string", "description": "匹配用户需求的理由"}
                        },
                        "required": ["index", "product_id", "score", "match_reason"]
                    },
                    "minItems": 1,
                    "maxItems": 10,
                    "description": "选中的产品列表，按评分降序排列"
                }
            },
            "required": ["selection_reason", "selected_products"]
        }

        # 构建消息
        messages = [
            self._llm.create_message(
                MessageType.SYSTEM,
                "你是一个资深购物顾问。你的任务是从候选商品列表中，根据用户的需求和约束条件，筛选出最合适的产品。"
                "请仔细分析每个产品的价格、品牌、参数、好评率等，给出合理的评分和选择理由。"
                "只返回 JSON 格式的结果，不要包含任何 Markdown 标记或额外解释。"
            ),
            self._llm.create_message(
                MessageType.USER,
                prompt
            )
        ]

        # 调用 LLM 进行筛选
        result = await self._llm.generate(messages, schema=select_schema)

        # 解析结果
        selected = self._parse_select_result(result, products)
        return selected

    def _build_select_prompt(self, products: List[Dict], intent: Dict,
                              user_query: str, search_plan: Dict) -> str:
        """构建产品筛选提示词"""
        # 格式化产品列表
        products_json = json.dumps(products, ensure_ascii=False, indent=2)

        # 构建用户意图摘要
        intent_summary = json.dumps(intent, ensure_ascii=False, indent=2)

        return (
            f"## 用户需求\n"
            f"原始查询: {user_query}\n"
            f"意图分析: {intent_summary}\n"
            f"搜索计划: {json.dumps(search_plan, ensure_ascii=False)}\n\n"
            f"## 候选商品列表（共 {len(products)} 件）\n"
            f"{products_json}\n\n"
            f"## 筛选要求\n"
            f"1. 根据用户的预算范围、使用场景、偏好品牌等约束条件进行综合评估\n"
            f"2. 考虑价格合理性、参数匹配度、好评率、品牌口碑等因素\n"
            f"3. 选出最符合用户需求的 10 个产品（如果不足 10 个则全部返回）\n"
            f"4. 为每个选中的产品给出评分（0-100）和匹配理由\n"
            f"5. 按评分降序排列"
        )

    def _parse_select_result(self, result: Dict, products: List[Dict]) -> List[Dict]:
        """解析 LLM 筛选结果，返回实际的产品对象"""
        try:
            # 从 LLM 响应中提取结构化数据
            choice = result.get("choices", [{}])[0]
            message = choice.get("message", {})

            # 尝试从 tool_calls 中提取（OpenAI 格式）
            if message.get("tool_calls"):
                tool_call = message["tool_calls"][0]
                arguments = tool_call["function"]["arguments"]
                data = json.loads(arguments)
            else:
                # 尝试从 content 中提取（本地模型格式）
                content = message.get("content", "")
                if not content:
                    return products[:10]  # 回退

                # 尝试直接解析 JSON
                try:
                    data = json.loads(content)
                except (json.JSONDecodeError, TypeError):
                    # 尝试从 markdown 代码块中提取
                    import re
                    md_match = re.search(r'```(?:json)?\s*\n?([\s\S]*?)```', content)
                    if md_match:
                        try:
                            data = json.loads(md_match.group(1).strip())
                        except (json.JSONDecodeError, TypeError):
                            # 尝试正则提取第一个 JSON 对象
                            json_match = re.search(r'\{[\s\S]*\}', content)
                            if json_match:
                                try:
                                    data = json.loads(json_match.group(0))
                                except (json.JSONDecodeError, TypeError):
                                    return products[:10]  # 回退
                            else:
                                return products[:10]  # 回退
                    else:
                        return products[:10]  # 回退

            # 从原始产品中提取选中的产品
            selected_products = []
            selected_info = data.get("selected_products", [])

            for item in selected_info:
                idx = item.get("index")
                if idx is not None and 0 <= idx < len(products):
                    product = products[idx].copy()
                    # 添加 LLM 评估信息
                    product["_score"] = item.get("score", 0)
                    product["_match_reason"] = item.get("match_reason", "")
                    selected_products.append(product)

            # 如果 LLM 返回的产品少于 10 个，用剩余产品补齐
            if len(selected_products) < 10:
                used_indices = {item.get("index") for item in selected_info}
                for i, product in enumerate(products):
                    if i not in used_indices:
                        product_copy = product.copy()
                        product_copy["_score"] = 0
                        product_copy["_match_reason"] = "未通过 LLM 筛选，但作为补充推荐"
                        selected_products.append(product_copy)
                    if len(selected_products) >= 10:
                        break

            return selected_products[:10]

        except Exception as e:
            print(f"[ProductAgent] 解析 LLM 筛选结果失败: {e}")
            return products[:10]  # 回退到原始排序

    def _build_planning_prompt(self, user_query: str) -> str:
        """构建规划提示词，包含详细的Schema约束"""
        schema_template = self._get_schema_template("planning")
        prompt_template = self._load_prompt('product_planning.txt')
        
        return prompt_template.format(
            user_query=user_query,
            schema_template=schema_template
        )

    def _get_schema_template(self, mode: str) -> str:
        """返回JSON Schema模板"""
        if mode == "planning":
            return '''
            {
                "agent_analyse": {
                    "type": "string",
                    "description": "对用户需求的深度分析，包括隐含预算、使用场景、目标人群等"
                },
                "intent_delta": {
                    "type": "string", 
                    "description": "修正后的自然语言需求描述，或提取的关键词组合（用于驱动搜索）"
                },
                "constraints": {
                    "type": "object",
                    "properties": {
                        "budget_min": {"type": "number", "description": "预算下限"},
                        "budget_max": {"type": "number", "description": "预算上限"},
                        "scenario": {"type": "string", "description": "使用场景"},
                        "recipient": {"type": "string", "description": "送礼对象"},
                        "brand_preference": {"type": "array", "items": {"type": "string"}},
                        "must_have_features": {"type": "array", "items": {"type": "string"}}
                    }
                },
                "information_gaps": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "缺失的关键信息列表"
                },
                "confidence_score": {
                    "type": "number",
                    "minimum": 0,
                    "maximum": 1,
                    "description": "对需求理解的置信度"
                }
            }
            '''
        return ""

    # ----------------------------------------------------------------
    # 核心模式 B: 咨询专家 (Consulting Mode - ReAct 交互)
    # ----------------------------------------------------------------
    async def consulting_step(self, user_input: str) -> Dict[str, Any]:
        """
        ReAct 循环：LLM 决定调哪个工具 → MCP 真实调用 → 结果回传 LLM → 生成回答。
        用于推荐后与用户交互产品细节（对比、价格、竞品等）。
        """
        tools_desc = self._build_tools_desc()
        intent = self._intent or ""
        history_str = self._format_conversation_history()

        max_iterations = 8
        max_empty = 3
        iteration = 0
        empty_count = 0
        tool_results = []

        while iteration < max_iterations:
            iteration += 1
            try:
                # 已执行结果
                executed = ""
                if tool_results:
                    executed = "## 已执行的工具调用\n"
                    for j, tr in enumerate(tool_results[-5:], 1):
                        executed += f"{j}. {tr['tool']}({json.dumps(tr['args'], ensure_ascii=False)})\n"
                        executed += f"   结果: {json.dumps(tr['result'], ensure_ascii=False, default=str)[:600]}\n\n"

                messages = [
                    self._llm.create_message(MessageType.SYSTEM, (
                        "你是资深购物顾问。用户看过推荐后来追问，判断意图：\n"
                        "1) 追问细节（如'支持人脸识别吗''电池多大'）→ 用推荐数据或工具查 → promote=answer\n"
                        "2) 对比竞品（如'和XX比怎么样'）→ 调 compare 工具 → promote=answer\n"
                        "3) 换推荐（如'换一个''还有别的吗'）→ promote=replan\n"
                        "4) 改需求（如'预算改成5000'）→ promote=replan\n"
                        "注意：追问细节时优先用已有推荐数据回答，不要轻易replan。"
                    )),
                    self._llm.create_message(MessageType.USER, (
                        f"## 已确认需求\n{intent}\n\n"
                        f"## 产品详细信息（来自搜索阶段，未截断）\n{json.dumps(self._search_results, ensure_ascii=False, default=str)[:3000]}\n\n"
                        f"## 对话历史\n{history_str[-1500:]}\n\n"
                        f"{executed}"
                        f"## 可用工具\n{tools_desc}\n\n"
                        f"## 用户消息\n{user_input}\n\n"
                        "先判断意图类型，再决定 promote 值。"
                    ))
                ]
                schema = {
                    "type": "object",
                    "properties": {
                        "tool_calls": {"type": "boolean", "description": "是否需要调用工具"},
                        "tool_name": {"type": "string", "description": "工具名称"},
                        "tool_arguments": {"type": "object", "description": "工具参数"},
                        "reply": {"type": "string", "description": "直接回答（如果不调工具）"},
                        "promote": {"type": "string", "description": "answer(已回答)/replan(换推荐)/need_tool(需要工具)"},
                    },
                    "required": ["tool_calls", "promote"]
                }
                result = await self._llm.generate(messages, schema=schema)
            except Exception as e:
                print(f"[ProductAgent] consulting LLM 出错 (iter={iteration}): {e}")
                return self._error_response(f"咨询阶段出错: {str(e)}")

            promote = result.get("promote", "need_tool")
            tool_calls = result.get("tool_calls", False)

            if promote == "answer" or (not tool_calls and result.get("reply")):
                reply = result.get("reply", "") or "抱歉，我暂时无法回答这个问题。"
                self.conversation_history.append({"role": "assistant", "content": reply})
                return {"mode": "consulting", "reply": reply, "candidates": [], "status": "success"}

            if promote == "replan":
                # 用户要重新推荐 → 回到 planning 模式
                self.state_machine.plan()
                return await self._handle_planning_mode(user_input)

            if promote == "need_tool" and tool_calls:
                tool_name = result.get("tool_name", "")
                tool_args = result.get("tool_arguments", {}) or {}

                if not tool_name or not tool_args:
                    empty_count += 1
                    if empty_count >= max_empty:
                        break
                    continue
                empty_count = 0

                print(f"[ProductAgent] consulting 调用工具: {tool_name}({json.dumps(tool_args, ensure_ascii=False)})")
                try:
                    mcp_result = self._mcp.call_tool(tool_name, tool_args)
                    if mcp_result is None:
                        mcp_result = {"info": "无结果"}
                    tool_results.append({"tool": tool_name, "args": tool_args, "result": mcp_result})
                except Exception as e:
                    tool_results.append({"tool": tool_name, "args": tool_args, "result": {"error": str(e)}})
                continue

            # 无法判断 → 直接让 LLM 回答
            reply = result.get("reply", "") or "抱歉，我暂时无法回答这个问题。"
            self.conversation_history.append({"role": "assistant", "content": reply})
            return {"mode": "consulting", "reply": reply, "candidates": [], "status": "success"}

        return self._error_response("咨询阶段达到最大轮次")

    # ----------------------------------------------------------------
    # 主循环：智能调度
    # ----------------------------------------------------------------
    async def run_agent(self, user_input: str, intent: Dict[str, Any] = None) -> Dict[str, Any]:
        """
        入口函数：智能调度模式切换

        Args:
            user_input: 用户原始输入（用于对话历史）
            intent: 从 GuideAgent 传递过来的已确认意图（category, core_need, constraints, budget）
                    如果提供，则跳过意图提取，直接进入 PLANNING 模式进行产品搜索和推荐
        """
        self.conversation_history.append({
            "role": "user",
            "content": user_input
        })

        # GuideAgent 首次传入 intent → 进入 PLANNING（仅 INIT 状态有效）
        if intent is not None and self.state_machine.INIT.is_active:
            self._safe_update_intent(intent)
            self.state_machine.plan()
            return await self._handle_planning_mode(user_input)

        if self.state_machine.PLANNING.is_active:
            return await self._handle_planning_mode(user_input)
        elif self.state_machine.DETAIL.is_active:
            # DETAIL 状态下由 consulting_step 自行处理 replan
            return await self._handle_consulting_mode(user_input)

    async def _handle_planning_mode(self, user_input: str) -> Dict[str, Any]:
        """PLANNING(搜索) → PROMOTE(推荐) → DETAIL(追问)"""
        # 阶段1+2：后台搜索
        search_result = await self.planning_step(user_input)
        if search_result.get("status") == "error":
            return search_result

        # 阶段3：比对需求，输出推荐 → PROMOTE
        rec_result = await self._promote_step()
        if rec_result.get("status") != "success":
            return rec_result

        # 推荐完成 → DETAIL
        try:
            self.state_machine.detail()
        except Exception:
            pass

        recs = rec_result.get("data", {}).get("recommendations", [])
        reply_parts = [f"{i}. {r.get('reason', '')} (ID: {r.get('product_id', '')})" for i, r in enumerate(recs, 1)]
        return {
            "mode": "planning",
            "reply": "为您找到以下推荐产品：\n" + "\n".join(reply_parts),
            "candidates": [{"name": r.get("product_id", ""), "reason": r.get("reason", "")} for r in recs],
            "status": "success"
        }

    async def _handle_consulting_mode(self, user_input: str) -> Dict[str, Any]:
        """处理咨询模式：ReAct 循环处理产品细节交互"""
        return await self.consulting_step(user_input)

    # ----------------------------------------------------------------
    # 辅助方法
    # ----------------------------------------------------------------
    def _validate_planning_output(self, data: Dict) -> Dict:
        """验证规划输出"""
        required_fields = ["agent_analyse", "intent_delta"]
        for field in required_fields:
            if field not in data:
                raise ValueError(f"缺少必需字段: {field}")
        
        # 确保约束字段存在
        if "constraints" not in data:
            data["constraints"] = {}
        
        if "information_gaps" not in data:
            data["information_gaps"] = []
        
        if "confidence_score" not in data:
            data["confidence_score"] = 0.5
            
        return data

    def _convert_delta_to_schema(self, planning_data: Dict) -> Dict:
        """将规划数据转换为咨询模式的需求单"""
        return {
            "agent_analyse": planning_data["agent_analyse"],
            "intent_delta": planning_data["intent_delta"],
            "constraints": planning_data.get("constraints", {}),
            "product_requests": self._generate_product_requests(planning_data)
        }

    def _generate_product_requests(self, planning_data: Dict) -> List[Dict]:
        """生成具体的产品请求列表"""
        requests = []
        constraints = planning_data.get("constraints", {})
        intent = planning_data["intent_delta"]
        
        # 根据意图生成不同的请求
        if "笔记本" in intent or "电脑" in intent:
            requests.append({
                "id": "req_001",
                "category": "laptop",
                "goal": f"搜索符合需求的笔记本电脑",
                "constraints": constraints,
                "parameters": ["price", "specs", "rating", "reviews"]
            })
        elif "手机" in intent:
            requests.append({
                "id": "req_002",
                "category": "smartphone",
                "goal": f"搜索符合需求的智能手机",
                "constraints": constraints,
                "parameters": ["price", "camera", "battery", "performance"]
            })
        else:
            # 通用搜索
            requests.append({
                "id": "req_003",
                "category": "general",
                "goal": f"搜索相关商品: {intent}",
                "constraints": constraints,
                "parameters": ["price", "rating"]
            })
        
        return requests

    def _should_switch_to_planning(self, user_input: str) -> bool:
        """判断是否应该切换到规划模式"""
        switch_triggers = ["重新", "换一个", "修改需求", "不是这个", "新的需求"]
        return any(trigger in user_input for trigger in switch_triggers)

    def _safe_update_intent(self, intent: Dict[str, Any]) -> None:
        """
        安全更新 ProductAgent 的 current_schema 字典（从 GuideAgent 传递过来）
        只更新非空字段，保留已有信息。
        更新完成后重新生成 intent 描述字符串。
        """
        if intent is None:
            return
        if self.current_schema is None:
            self.current_schema = {}
        for key, value in intent.items():
            if value is None:
                continue
            if isinstance(value, str) and value.strip():
                self.current_schema[key] = value.strip()
            elif isinstance(value, (int, float)):
                self.current_schema[key] = value
            elif isinstance(value, list):
                if key not in self.current_schema:
                    self.current_schema[key] = []
                for item in value:
                    if item and isinstance(item, str) and item.strip():
                        if item.strip() not in self.current_schema[key]:
                            self.current_schema[key].append(item.strip())
            elif isinstance(value, dict):
                self.current_schema[key] = value
        # 重新生成 intent 描述字符串
        self._intent = self._intent_fix()

    def _is_planning_switch_trigger(self, user_input: str) -> bool:
        """判断是否触发切换回规划模式"""
        triggers = ["换一个", "修改需求", "重新规划", "换个方向", "不是这个"]
        return any(trigger in user_input for trigger in triggers)

    def _error_response(self, message: str) -> Dict:
        """生成错误响应"""
        return {
            "mode": "error",
            "status": "error",
            "message": message,
            "data": {}
        }

    async def _handle_llm_error(self, user_query: str, retry_count: int, error_msg: str) -> Dict:
        """处理LLM错误，尝试重试"""
        if retry_count < self.max_retries - 1:
            return await self.planning_step(retry_count + 1)
        return self._error_response(f"LLM处理失败: {error_msg}")

    # 以下为提取方法的具体实现（可根据需要完善）
    def _extract_summary(self, text: str) -> str:
        return text[:200] + "..." if len(text) > 200 else text
    
    def _extract_top_picks(self, text: str) -> List[str]:
        # 简单提取示例
        return ["商品A", "商品B"] if "推荐" in text else []
    
    def _extract_comparison_table(self, text: str) -> Dict:
        return {"headers": ["参数", "商品A", "商品B"], "rows": []}
    
    def _extract_risks(self, text: str) -> List[str]:
        return ["风险提示1", "风险提示2"] if "风险" in text else []
    
    def _extract_buying_advice(self, text: str) -> str:
        return "建议在促销季购买"
    
    def _prepare_consulting_context(self, request_schema: Dict) -> Dict:
        return {
            **request_schema,
            "conversation_history": self.conversation_history[-3:]  # 最近3轮对话
        }
    
    def _update_schema_with_follow_up(self, user_input: str) -> Dict:
        """根据追问更新需求单"""
        updated = self.current_schema.copy() if self.current_schema else {}
        updated["follow_up"] = user_input
        return updated
    
    def _request_clarification(self, planning_data: Dict) -> Dict:
        """请求用户补充信息"""
        gaps = planning_data.get("information_gaps", [])
        if gaps:
            return {
                "mode": "clarification",
                "status": "need_more_info",
                "message": f"需要您补充以下信息: {', '.join(gaps)}",
                "data": planning_data
            }
        return {
            "mode": "clarification",
            "status": "confirm",
            "message": "已理解您的需求，是否继续?",
            "data": planning_data
        }

    def Run(self, user_input: str, intent: Dict[str, Any] = None) -> tuple:
        """
        同步包装器：供 gRPC 服务调用
        返回 (reply, candidates) 元组
        """
        import asyncio
        try:
            result = asyncio.run(self.run_agent(user_input, intent=intent))
        except Exception as e:
            print(f"[ProductAgent] 运行时错误: {e}")
            return "", []
        reply = result.get("reply", "")
        candidates = result.get("candidates", [])
        return reply, candidates


if __name__ == "__main__":
    import unittest
    from unittest.mock import MagicMock, patch

    class TestProductAgent(unittest.TestCase):
        def setUp(self):
            self.config = {
                "templates_dir": "templates"
            }
            self.agent = ProductAgent.__new__(ProductAgent)
            self.agent.config = self.config
            self.agent._llm = MagicMock()
            self.agent._mcp = MagicMock()
            self.agent._tool_list = []
            self.agent.state_machine = MagicMock()
            self.agent._prompts_dir = "templates"  # 添加缺失的属性
            
            # 初始化其他属性
            self.agent._user_id = None
            self.agent._session_id = None
            self.agent.current_schema = None
            self.agent.conversation_history = []
            self.agent._intent = None
            self.agent.short_memory = []
            self.agent.max_retries = 3
            self.agent.user_profile = ""  # 初始化为空字符串，避免 replace 报错
            self.agent.planning_str = ""  # 添加缺失的属性


        def test_load_user_profile_success(self):
            """测试正常路径：用户画像加载成功"""
            self.agent._user_id = "test_user_123"
            
            result = self.agent._load_user_profile({"user_id": "test_user_123"})
            
            self.assertTrue(result)
            self.agent._mcp.connect.assert_called_once()
            self.agent._mcp.call_tool.assert_called_with("load_user_profile", {"user_id": "test_user_123"})
            self.assertIsNotNone(self.agent.user_profile)

        def test_load_user_profile_missing_id(self):
            """测试边界值：缺少 user_id"""
            self.agent._user_id = None
            
            result = self.agent._load_user_profile({})
            
            self.assertFalse(result)
            self.assertEqual(self.agent.user_profile, "无用户画像信息")

        def test_load_user_profile_exception(self):
            """测试异常路径：MCP 调用失败"""
            self.agent._user_id = "test_user_123"
            self.agent._mcp.connect.side_effect = Exception("Connection refused")
            
            result = self.agent._load_user_profile({"user_id": "test_user_123"})
            
            self.assertFalse(result)
            self.assertEqual(self.agent.user_profile, "加载用户画像失败")

        def test_planning_step_success(self):
            """测试正常路径：planning_step 成功"""
            # Mock LLM 返回符合 schema 的结果
            async def mock_generate(*args, **kwargs):
                return {
                    "step1": "Search products",
                    "step2": "Filter by price",
                    "step3": "Sort by rating",
                    "step4": "Return top 10"
                }
            self.agent._llm.generate = mock_generate
            
            # Mock runtime_prepare 返回非空字符串
            self.agent.runtime_prepare = MagicMock(return_value="Test Prompt")

            result = asyncio.run(self.agent.planning_step())
            
            self.assertEqual(result["status"], "success")

        def test_planning_step_retry_logic(self):
            """测试边界值/异常路径：验证分阶段重试逻辑修复"""
            call_count = 0
            
            async def mock_generate(*args, **kwargs):
                nonlocal call_count
                call_count += 1
                if call_count <= 2:
                    raise Exception(f"Error {call_count}")
                return {"step1": "Success", "step2": "OK", "step3": "OK", "step4": "OK"}
            
            self.agent._llm.generate = mock_generate

            result = asyncio.run(self.agent.planning_step())
            
            # 验证虽然前两次失败，但最终成功返回
            self.assertEqual(result["status"], "success")

        def test_planning_step_total_failure(self):
            """测试异常路径：LLM 始终失败"""
            async def mock_generate(*args, **kwargs):
                raise Exception("Persistent Error")
            
            self.agent._llm.generate = mock_generate

            result = asyncio.run(self.agent.planning_step())
            
            self.assertEqual(result["status"], "error")

    # 运行测试
    unittest.main()

import os
import json
from pathlib import Path
import sys
current_dir = Path(__file__).resolve().parent
package_path = current_dir.parent.parent
sys.path.append(str(package_path))

from typing import Optional
from core.util.load import load_text
from core.guide_agent.guide_statemachine import GuideStateMachine
from core.llm_client.llm_client import LLMClient,ModelProvider,MessageType
from mcp_servers.client import MCPClient

# ── 确认意图检测（embedding）──
_CONFIRM_PHRASES = [
    "正确", "确认", "是的", "没错", "可以", "好的", "行", "没问题",
    "对", "就这个", "就这样", "对的", "确认无误", "没问题了",
    "就按这个来", "是的确认", "嗯嗯", "就这些", "没有其他要求了",
]
_CONFIRM_THRESHOLD = 0.78  # 余弦相似度阈值


class GuideAgent:
    def __init__(self, config: dict):
        self.config = config
        self.state_machine = GuideStateMachine()
        self._llm = LLMClient.from_config(config)
        self._mcp = MCPClient.from_config(config)

        self._current_target:str = ""       # 当前目标
        self.context = {}                   # 上下文信息，存储转换状态的结果
        self._history = []                  # 历史对话记录
        self._prompt:str = ""               # 当前 prompt
        self._product_prompt:str = ""       # 产品 prompt，来自mcp服务端
        self._short_memory = []             # 短期记忆，存储llm分析以及历史信息
        self._tool_list = []                # 工具列表，来自mcp服务端
        self._user_id = ""                  # 用户唯一标识，用于区分不同用户的短期记忆和历史记录
        self._session_id = ""               # 会话唯一标识，用于区分不同会话的短期记忆和历史记录
        self._short_memory_limit = 10       # 短期记忆的最大长度，超过则删除最早的记忆
        self._user_profile:str = None       # 用户画像，存储用户的基本信息和偏好
        self._system_prompt = load_text(config.get("system_prompt", ""))  # 系统提示词，提供给llm的系统角色信息
        self.intent = {}                          # 用户意图，存储用户的核心需求、约束条件和预算信息

        # embedding 确认检测器（延迟初始化）
        self._confirm_embeddings = None
        self._confirm_norms = None

        self._register_before_callbacks()   # 注册状态机的 before 状态回调函数，用于在状态转换前执行一些操作

        self._reply_schema = {
            "type": "object",
            "properties": {
                "agent_analyse": {"type": "string"},
                "reply": {"type": "string"}
            },
            "required": ["agent_analyse", "reply"]
        }

        # 通用 intent_delta 子结构 —— 每一步都收集，逐步累积
        self._intent_delta_schema = {
            "type": "object",
            "properties": {
                "category": {"type": "string", "description": "产品类型，如'手机'/'笔记本'/'耳机'，未知则为空"},
                "core_need": {"type": "string", "description": "核心需求一句话概括，如'需要拍照好的手机'"},
                "budget_min": {"type": "number", "description": "预算下限（元），未知则不填"},
                "budget_max": {"type": "number", "description": "预算上限（元），未知则不填"},
                "constraints": {"type": "array", "items": {"type": "string"}, "description": "约束条件，如'安卓系统'、'续航长'"},
                "preferences": {"type": "array", "items": {"type": "string"}, "description": "用户偏好，如'华为'、'轻薄'、'人像拍摄'"},
            }
        }

        self._ask_schema = {
            "type": "object",
            "properties": {
                "agent_analyse": {"type": "string", "description": "对用户当前输入的分析"},
                "reply": {"type": "string", "description": "给用户的回复"},
                "intent_delta": self._intent_delta_schema,
            },
            "required": ["agent_analyse", "reply", "intent_delta"]
        }

        self._intent_schema = {
            "type": "object",
            "properties": {
                "agent_analyse": {"type": "string"},
                "reply": {"type": "string"},
                "intent_delta": self._intent_delta_schema,
            },
            "required": ["agent_analyse", "reply", "intent_delta"]
        }

        self._observe_schema = {
            "type": "object",
            "properties": {
                "agent_analyse": {"type": "string"},
                "reply": {"type": "string"},
                "intent_delta": self._intent_delta_schema,
            },
            "required": ["agent_analyse", "reply", "intent_delta"]
        }

        self._observe_intent_schema = {
            "type": "object",
            "properties": {
                "agent_analyse": {"type": "string"},
                "reply": {"type": "string"},
                "intent_delta": self._intent_delta_schema,
            },
            "required": ["agent_analyse", "reply", "intent_delta"]
        }

    def _register_before_callbacks(self):
        """
        注册状态机的 before 状态回调函数，用于在状态转换前执行一些操作
        """
        self.state_machine._before_ask_callback = self._before_ask
        self.state_machine._before_detail_callback = self._before_detail

    def _before_ask(self):
        """
        转换ASK current目标（同步方法，仅做字符串赋值）
        """
        self._current_target = "用户还没有确定产品或场景，你需要引导用户输出购买的核心需求，了解用户预算以及产品关注点，你可以根据需求给出产品类型建议，但需求产品必须从用户回答中提取，不能自己编造"
        self._short_memory.clear()
        
    def _before_detail(self):
        self._current_target=f"当前已经进入最终汇总阶段，请根据用户最后的输入补充和修改{self.intent}生成最终的购买清单"

    def set_user_id(self, user_id: str, session_id:str):
        """
        设置用户唯一标识和会话唯一标识，用于区分不同用户和会话的短期记忆和历史记录
        """
        self._user_id = user_id
        self._session_id = session_id

    async def _get_user_profile(self)-> bool:
        """
        获取用户画像
        """
        try:
            self._mcp.connect()
            self._mcp.call_tool("load_user_profile", {"user_id": self._user_id})
            return True
        except Exception as e:
            print(f"Error occurred while fetching user profile: {e}")
        return False

    async def _runtime_prepare(self,prompt_dict: dict) -> str:
        """
        运行时准备工作，包括加载prompt模版、拼接用户画像、当前目标、历史会话、短期记忆等信息，生成最终的prompt
        """
        # 加载prompt模版
        prompt_dir = prompt_dict.get("template", "")
        prompt_template = load_text(prompt_dir)

        # 拼接用户画像
        if self._user_profile is None:
            await self._get_user_profile()
        user_profile_str = self._user_profile or prompt_dict.get("user_profile") or "无用户画像"

        # 拼接当前目标
        current_target_str = self._current_target if self._current_target or prompt_dict.get("current_target") else "无当前目标"

        # 拼接历史会话
        history_str = ""
        if self._history or prompt_dict.get("history"):
            for i, (role, content) in enumerate(self._history):
                history_str += f"\n历史会话 {i+1}:\n角色: {role}\n内容: {content}"
        else:
            history_str = "\n历史会话: 无历史会话"

        if self._product_prompt or prompt_dict.get("product_prompt"):
            product_prompt_str = self._product_prompt or prompt_dict.get("product_prompt")
        else:
            product_prompt_str = "\n"

        # 拼接短期记忆
        if self._short_memory or prompt_dict.get("short_memory"):
            if len(self._short_memory) > self._short_memory_limit:
                self._short_memory = self._short_memory[-self._short_memory_limit:]
            short_memory_str = "\n".join([f"{item}" for i, item in enumerate(self._short_memory)])
        else:
            short_memory_str = "\n当前为任务初始阶段"

        # 每一步都把已收集的意图作为记忆注入 prompt
        intent_str = self._intent_memory()

        # 替换prompt模版中的占位符
        prompt_template = prompt_template.replace("{user_profile}", user_profile_str)
        prompt_template = prompt_template.replace("{current_target}", current_target_str)
        prompt_template = prompt_template.replace("{product_prompt}", product_prompt_str)
        prompt_template = prompt_template.replace("{history}", history_str)
        prompt_template = prompt_template.replace("{short_memory}", short_memory_str)
        prompt_template = prompt_template.replace("{intent}", intent_str)
        return prompt_template
    
    def reset(self):
        """
        重置状态准备加载新的任务，清空当前目标、历史会话、短期记忆、工具列表、用户画像、用户唯一标识和会话唯一标识
        """
        self.state_machine.reset()
        self.intent = {}
        self._current_target = ""
        self._history = []
        self._prompt = ""
        self._short_memory = []
        self._tool_list = []
        self._product_prompt = ""
        self._user_profile = None
        self._user_id = ""
        self._session_id = ""
        
    def _init_confirm_embeddings(self):
        """延迟初始化确认短语的 embedding 向量（避免启动时阻塞）"""
        if self._confirm_embeddings is not None:
            return
        try:
            from openai import OpenAI
            api_key = os.getenv("EMBEDDING_API_KEY", "")
            base_url = os.getenv("EMBEDDING_BASE_URL", "https://api.siliconflow.cn/v1")
            if not api_key or "your-key" in api_key:
                return  # 无有效 key，回退到关键词匹配
            client = OpenAI(api_key=api_key, base_url=base_url, timeout=10)
            resp = client.embeddings.create(model="BAAI/bge-m3", input=_CONFIRM_PHRASES)
            self._confirm_embeddings = [d.embedding for d in resp.data]
            self._confirm_norms = [sum(v*v for v in e) ** 0.5 for e in self._confirm_embeddings]
            print(f"[GuideAgent] ✅ 确认意图 embedding 已初始化 ({len(_CONFIRM_PHRASES)} 短语)")
        except Exception as e:
            print(f"[GuideAgent] ⚠️ embedding 初始化失败，回退到关键词匹配: {e}")

    def _is_confirm(self, user_input: str) -> bool:
        """用 embedding 余弦相似度判断用户是否在确认需求"""
        # 尝试 embedding 检测
        if self._confirm_embeddings is None:
            self._init_confirm_embeddings()
        if self._confirm_embeddings is not None:
            try:
                from openai import OpenAI
                api_key = os.getenv("EMBEDDING_API_KEY", "")
                base_url = os.getenv("EMBEDDING_BASE_URL", "https://api.siliconflow.cn/v1")
                client = OpenAI(api_key=api_key, base_url=base_url, timeout=10)
                resp = client.embeddings.create(model="BAAI/bge-m3", input=user_input)
                user_vec = resp.data[0].embedding
                user_norm = sum(v*v for v in user_vec) ** 0.5
                if user_norm == 0:
                    return False
                for i, phrase_vec in enumerate(self._confirm_embeddings):
                    dot = sum(a*b for a, b in zip(user_vec, phrase_vec))
                    sim = dot / (user_norm * self._confirm_norms[i])
                    if sim >= _CONFIRM_THRESHOLD:
                        print(f"[GuideAgent] ✅ embedding 检测到确认 ({_CONFIRM_PHRASES[i]}, sim={sim:.3f})")
                        return True
            except Exception as e:
                print(f"[GuideAgent] ⚠️ embedding 检测失败: {e}")

        # 回退：关键词匹配
        input_lower = user_input.lower().strip()
        confirm_kw = ["正确", "确认", "是的", "没错", "可以", "好的", "行", "没问题",
                      "对", "就这个", "就这样", "对的", "ok", "yes"]
        skip_kw = ["直接推荐", "给我推荐", "推荐吧", "不用问了", "不需要", "别问了",
                   "直接给我", "快推荐", "就这些", "没有其他", "就这样吧"]
        for kw in confirm_kw + skip_kw:
            if kw in input_lower:
                print(f"[GuideAgent] ✅ 关键词检测到确认/跳过 ({kw})")
                return True
        return False

    def _intent_memory(self) -> str:
        """将已收集的意图信息格式化为 LLM 记忆，每一步都注入 prompt"""
        if not self.intent or not any(self.intent.values()):
            return "（暂无已收集的需求信息）"

        parts = ["## 已收集的用户需求（请基于此继续引导，不要重复询问已明确的信息）"]
        if self.intent.get("category"):
            parts.append(f"- 产品类型: {self.intent['category']}")
        if self.intent.get("core_need"):
            parts.append(f"- 核心需求: {self.intent['core_need']}")
        bmin = self.intent.get("budget_min")
        bmax = self.intent.get("budget_max")
        if bmin is not None or bmax is not None:
            parts.append(f"- 预算: {bmin or '?'} - {bmax or '?'} 元")
        if self.intent.get("constraints"):
            parts.append(f"- 约束: {', '.join(str(c) for c in self.intent['constraints'])}")
        if self.intent.get("preferences"):
            parts.append(f"- 偏好: {', '.join(str(p) for p in self.intent['preferences'])}")
        return "\n".join(parts)

    def _intent_fix(self)->str:
        """汇总已收集的用户意图，生成确认文本"""
        response = "以下是汇总后，您的购物需求：\n"
        if self.intent.get("category"):
            response += f"产品类型: {self.intent['category']}\n"
        if self.intent.get("core_need"):
            response += f"核心需求: {self.intent['core_need']}\n"
        # 扁平 budget 字段
        bmin = self.intent.get("budget_min")
        bmax = self.intent.get("budget_max")
        if bmin is not None or bmax is not None:
            min_s = str(int(bmin)) if bmin is not None else "?"
            max_s = str(int(bmax)) if bmax is not None else "?"
            response += f"预算范围: {min_s} - {max_s} 元\n"
        if self.intent.get("constraints") and isinstance(self.intent["constraints"], list):
            response += f"约束条件: {', '.join(str(c) for c in self.intent['constraints'])}\n"
        if self.intent.get("preferences") and isinstance(self.intent["preferences"], list):
            response += f"偏好倾向: {', '.join(str(p) for p in self.intent['preferences'])}\n"
        response += "请确认以上信息是否正确，如有误请补充说明。"
        return response

    def _ready_to_done(self):
        # 只要有 category 就认为需求已基本确认（budget 可选）
        return bool(self.intent.get("category"))

    def _safe_update_intent(self, intent_delta):
        """安全更新 intent，兼容模型返回 JSON 字符串或 dict"""
        if isinstance(intent_delta, dict):
            self.intent.update(intent_delta)
        elif isinstance(intent_delta, str):
            # 模型可能把嵌套对象序列化为 JSON 字符串
            try:
                parsed = json.loads(intent_delta)
                if isinstance(parsed, dict):
                    self.intent.update(parsed)
                    return
            except (json.JSONDecodeError, TypeError):
                pass
            print(f"[GuideAgent] ⚠️ intent_delta 字符串无法解析为 JSON: {intent_delta[:200]}")
        else:
            print(f"[GuideAgent] ⚠️ intent_delta 不是 dict/str，已忽略: {type(intent_delta).__name__}")

    async def run(self,message:Optional[str]=None)->tuple[bool,str]:
        """
        运行引导任务，返回是否成功和输出结果
        """
        if message:
            self._history.append(("user", message))
        
        if self.state_machine.INIT.is_active:
            # 初始化状态，确认当前任务，加载模版
            self._current_target = "用户刚进入引导任务，尚未确认产品或场景，请输出欢迎词并引导用户确认产品或场景"
            prompt = await self._runtime_prepare({"template": self.config.get("INIT_PROMPT", "")})

            try:
                response = await self.chat_schema(prompt=prompt,system_prompt=self._system_prompt,schema=self._reply_schema)
                if response.get("reply") is None:
                    self.state_machine.fail()
                    return False, "LLM生成响应失败: 未返回欢迎词"
            except Exception as e:
                self.state_machine.fail()
                return False, f"LLM生成响应失败: {e}"
            
            self._history.append(("agent", response["reply"]))
            self._short_memory.append(response["agent_analyse"])
            self.state_machine.ask()
            return True, response["reply"]

        elif self.state_machine.ASKING.is_active:
            # 用户明确要跳过 → 强制进入汇总，不再追问
            if self._is_confirm(message):
                # 直接跳到 ASKING_FINAL → OBSERVING → DETAIL
                prompt = await self._runtime_prepare({"template": self.config.get("ASKING_FINAL_PROMPT", "")})
                try:
                    response = await self.chat_schema(prompt=prompt, system_prompt=self._system_prompt, schema=self._intent_schema)
                except Exception as e:
                    self.state_machine.fail()
                    return False, f"LLM生成响应失败: {e}"
                if response.get("intent_delta"):
                    self._safe_update_intent(response.get("intent_delta", {}))
                self.state_machine.detail()
                return True, self._intent_fix()

            # 与用户交谈，确认产品或者场景
            prompt = await self._runtime_prepare({"template": self.config.get("ASKING_PROMPT", "")})

            try:
                response = await self.chat_schema(prompt=prompt,system_prompt=self._system_prompt,schema=self._ask_schema)
            except Exception as e:
                self.state_machine.fail()
                return False, f"LLM生成响应失败: {e}"

            """
            模型输出“status”为True时，表示用户需求分析完成，可以进入OBSERVING状态；
            模型输出“status”为False时，表示用户需求分析未完成，需要继续在ASKING状态与用户交谈，直到分析完成。
            """
            if response.get("status"):   # 用户需求分析完成，进入OBSERVING状态
                prompt = await self._runtime_prepare({"template": self.config.get("ASKING_FINAL_PROMPT", "")})
                try:
                    response = await self.chat_schema(prompt=prompt,system_prompt=self._system_prompt,schema=self._intent_schema)
                except Exception as e:
                    self.state_machine.fail()
                    return False, f"LLM生成响应失败: {e}"

                if response.get("intent_delta") is None:
                    self.state_machine.fail()
                    return False, "LLM生成响应失败: 未返回意图增量"
                self._safe_update_intent(response.get("intent_delta", {}))

                # === 原 _before_observe 逻辑：MCP 获取产品 prompt → sm.observe() → LLM 生成观察结果 ===
                # (a) MCP 调用获取产品 prompt（错误容忍，不阻塞状态转换）
                try:
                    self._mcp.connect()
                    raw_prompt = self._mcp.call_tool("find_product_prompt", {"product": self.intent.get("category", "")})
                    # MCP 调用失败时返回 error dict，需要转为字符串以防 replace() 报错
                    self._product_prompt = raw_prompt if isinstance(raw_prompt, str) else str(raw_prompt)
                except Exception as e:
                    print(f"Error occurred while fetching product prompt: {e}")

                # (b) 同步转换到 OBSERVING 状态
                self.state_machine.observe()
                self._current_target = f"用户已经确认产品或场景{self.intent.get('category', '')}，你需要根据产品参数模版，用户对具体参数的需求，不要求所有参数都必须提供，用户不明确则自动跳过."

                # (c) 此时 OBSERVING.is_active == True，_runtime_prepare 会包含 intent_str
                obs_prompt = await self._runtime_prepare({ "template": self.config.get("OBSERVING_PROMPT", "") })
                try:
                    obs_response = await self.chat_schema(prompt=obs_prompt, system_prompt=self._system_prompt, schema=self._reply_schema)
                except Exception as e:
                    self.state_machine.fail()
                    return False, f"LLM生成响应失败: {e}"

                # (d) 存储观察结果到 context 和 memory
                if obs_response.get("reply") is None:
                    self.context["observing_reply"] = None
                else:
                    self.context["observing_reply"] = obs_response["reply"]
                self._short_memory.append(obs_response["agent_analyse"])
                self._history.append(("agent", obs_response["reply"]))
                # === _before_observe 逻辑结束 ===

                if self.context.get("observing_reply") is None:
                    return False, "LLM生成响应失败: 未返回观察结果"
                else:
                    return True, self.context["observing_reply"]
                
            else:                        # 用户需求分析未完成，继续在ASKING状态
                self.state_machine.ask()
                if response.get("reply") is None:
                    self.state_machine.fail()
                    return False, "LLM生成响应失败: 未返回回复"
                else:
                    self._history.append(("agent", response["reply"]))
                    self._short_memory.append(response["agent_analyse"])
                    return True, response.get("reply")

        elif self.state_machine.OBSERVING.is_active:
            # 用户明确要跳过 → 直接进汇总
            if self._is_confirm(message):
                self.state_machine.detail()
                return True, self._intent_fix()

            # 产品具体细节打磨，设置提问上线和提问技巧，最终输出概括json
            prompt_dict = {
                "template": self.config.get("OBSERVING_PROMPT", "")
            }
            prompt = await self._runtime_prepare(prompt_dict)
            try:
                response = await self.chat_schema(prompt=prompt,system_prompt=self._system_prompt,schema=self._observe_schema)
            except Exception as e:
                self.state_machine.fail()
                return False, f"LLM生成响应失败: {e}"

            if response.get("status") is False:
                self._short_memory.append(response.get("agent_analyse", ""))
                self._history.append(("agent", response.get("reply", "")))
                return True, response.get("reply", "")
            else:
                try:
                    prompt_dict = {
                        "template": self.config.get("OBSERVING_FINAL_PROMPT", "")
                    }
                    prompt = await self._runtime_prepare(prompt_dict)
                    response = await self.chat_schema(prompt=prompt,system_prompt=self._system_prompt,schema=self._observe_intent_schema)
                except Exception as e:
                    self.state_machine.fail()
                    return False, f"LLM生成响应失败: {e}"
                
                if response.get("intent_delta") is None:
                    self.state_machine.fail()
                    return False, "LLM生成响应失败: 未返回意图增量"
                self._safe_update_intent(response.get("intent_delta", {}))
                reply = self._intent_fix()
                self._history.append(("agent", reply))
                self._short_memory.append(response.get("agent_analyse", ""))
                self.state_machine.detail()
                return True, reply

        elif self.state_machine.DETAIL.is_active:
            # 用 embedding 判断用户是否在确认（而非补充新需求）
            if self._is_confirm(message):
                if self._ready_to_done():
                    self.state_machine.done()
                    return True, "✅ 需求已确认！正在为您推荐产品..."
                else:
                    self.state_machine.fail()
                    return False, "引导任务未完成，缺少必要信息"

            # 用户提供了新信息 → 调用 LLM 更新意图
            prompt_dict = {"template": self.config.get("DETAIL_PROMPT", "")}
            prompt = await self._runtime_prepare(prompt_dict)
            llm_response = await self.chat_schema(prompt=prompt, system_prompt=self._system_prompt, schema=self._observe_intent_schema)
            if not llm_response:
                self.state_machine.fail()
                return False, "LLM生成响应失败"

            self._safe_update_intent(llm_response.get("intent_delta", {}))
            if not self._ready_to_done():
                self.state_machine.fail()
                return False, "引导任务未完成，缺少必要信息"
            self.state_machine.done()
            return True, self._intent_fix()

        elif self.state_machine.DONE.is_active:
            # 需求已确认，返回汇总后的购买清单
            return True, self._intent_fix()

        # 防御性 fallback：正常流程不应到达此处
        return False, "未知状态: 状态机未正确初始化"

    def Run(self, message: dict) -> tuple[bool, str]:
        """
        同步包装器：供 gRPC 服务调用
        注意：此方法会阻塞，实际应使用异步 run() 方法
        """
        import asyncio
        return asyncio.get_event_loop().run_until_complete(self.run(message.get("content", "")))

    def load_session(self,userid:str,sessionid:str)->bool:
        kwargs = {"user_id":userid,"session_id":sessionid}
        try:
            self._mcp.connect()
            state =self._mcp.call_tool(tool_name="load_guide_state",kwargs=kwargs)
        except Exception as e:
            self.state_machine.fail()
            return False
        self.state_machine = state.get("state_machine")
        self._current_target = state.get("current_target")
        self._history = state.get("history")
        self._short_memory = state.get("short_memory")
        self._user_id = state.get("user_id")
        self._session_id = state.get("session_id")
        return True

    def save_state(self):
        """
        保存当前状态，包括状态机状态、当前目标、历史会话、短期记忆、工具列表、用户画像、用户唯一标识和会话唯一标识
        """
        state = {
            "state_machine": self.state_machine.current_state_value.name,
            "current_target": self._current_target,
            "history": self._history,
            "short_memory": self._short_memory,
            "user_id": self._user_id,
            "session_id": self._session_id
        }
        try:
            self._mcp.connect()
            self._mcp.call_tool(tool_name= "save_guide_state", arguments=state)
        except:
            return False
        return True
    
    async def chat_schema(self, prompt: str, system_prompt: str, schema: dict) -> dict:
        """
        使用llm的chat_json方法，生成符合schema的响应
        """
        messages = [
            self._llm.create_message(MessageType.SYSTEM, system_prompt),
            self._llm.create_message(MessageType.USER, prompt)
        ]
        try:
            result = await self._llm.generate(messages, schema)
            # _llm.generate() 内部已调用 _parase_schema 解析，无需重复调用
        except Exception as e:
            print(f"Error generating structured response: {e}")
            raise

        return result

if __name__ == "__main__":
    import asyncio
    async def main():
        """异步主函数"""
        from mcp_servers.client import MCPClient
        from mcp_servers.server import init_agents
        import logging
        logging.basicConfig(level=logging.INFO)
        import os

        # 在程序开头设置环境变量
        os.environ["JD_APP_KEY"] = "9e27696abcd41f3fd394b6dd72e4c120"
        os.environ["JD_APP_SECRET"] = "8067864bface45f9ada23159deefa375"
        config = {
            "system_prompt": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_system.txt",
            "INIT_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_init.txt",
            "ASKING_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_ask.txt",
            "ASKING_FINAL_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_ask_intent.txt",
            "OBSERVING_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_observe.txt",
            "OBSERVING_FINAL_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_observe_intent.txt",
            "DETAIL_PROMPT": "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/prompts/guide_detail.txt",
            "provider": ModelProvider.OPENAI,
            "base_url": "http://localhost:1234/v1",
            "model": "google/gemma-4-e4b",
            "api_key": "518678",
            "timeout": 60,
            "temperature": 0.3,
            "max_tokens": 2048,
            "max_retries": 3,
            "mcp": {
                "server_url": "http://localhost:8888/sse",
                "port": 8888,
                "host": "0.0.0.0",
                "role": "guide_agent"
            }
        }
        if not init_agents(config):
            print("初始化 MCP 服务失败")
            exit(1)
        agent = GuideAgent(config)

        # ✅ 使用 await 调用异步 run
        success, output = await agent.run()
        print(f"Assistant: {output}")
        
        # 循环接收用户输入
        while True:
            try:
                user_input = input("\n你: ").strip()
                
                if user_input.lower() in ['quit', 'exit', 'q']:
                    print("👋 感谢使用购物导购助手，再见！")
                    break
                
                if not user_input:
                    continue
                
                # ✅ 使用 await 调用异步 run
                success, output = await agent.run(user_input)
                
                if success:
                    print(f"Assistant: {output}")
                else:
                    print(f"❌ 错误: {output}")
                    
            except KeyboardInterrupt:
                print("\n\n👋 程序被中断，再见！")
                break
            except Exception as e:
                print(f"❌ 发生错误: {e}")
    
    asyncio.run(main())
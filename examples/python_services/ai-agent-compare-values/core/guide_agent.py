"""
导购Agent — 用户需求分析 + 用户摘要管理 + 短期记忆

重构自 AnalysisAgent，通过 MCP 工具调用长期存储和会话持久化。

生命周期:
    1. session_start → load_profile (从 MCP 加载用户摘要，一句话画像)
    2. 每轮对话 → analyze_l1 (增量分析 + 追问，传入用户摘要做参考)
    3. 3-5轮后 → finalize (生成当前会话信息元，供 product_agent 使用)
    4. session_end → update_user_summary (用本会话数据更新用户摘要) + save_session

使用示例:
    from core import ShoppingGuideAgent, LLMClient
    from core.config import load_config

    cfg = load_config()
    llm = LLMClient(...)
    mcp = MCPClient(group="memory")  # 只访问 memory 组工具

    agent = ShoppingGuideAgent(llm=llm, mcp=mcp, config=cfg)
    agent.start_session("sess_001", context="选购笔记本")

    l1 = agent.analyze_l1([{"role": "user", "content": "想买8000左右的笔记本"}])
    # → {profile_delta, intent_delta, gaps, follow_up_questions}
"""

import json
import os
import re
import time
from typing import Optional, Any
from tools.json_parse import _construct_fallback_json

from memory.models import MemoryNode
from memory.short_memory import ShortMemoryBuffer
from core.llm_client import LLMClient
from core.share_utils import load_text_file, build_kwargs_with_user_id


class ShoppingGuideAgent:
    """
    导购Agent — 分析用户购买需求、管理用户画像、维护短期记忆。
    """

    def __init__(
        self,
        llm: LLMClient,
        mcp: Any = None,  # MCPClient (memory 组)
        config: Optional[dict] = None,
        short_memory_volume: int = 100,
        l1_temperature: float = 0.1,
        finalize_temperature: float = 0.1,
    ):
        """
        llm: LLMClient 实例
        mcp: MCP 客户端（memory 组工具），不传则降级为本地存储
        config: YAML 配置字典（用于读取 prompt 路径）
        """
        cfg = config or {}
        self.llm = llm
        self._mcp = mcp
        self._session_id = ""

        # prompt 路径
        self._config_dir = os.path.dirname(os.path.abspath(
            os.getenv("CONFIG_PATH", "config.yaml")
        ))
        prompt_cfg = cfg.get("prompts", {})
        print(prompt_cfg)
        defaults = {
            "guide_system": "prompts/guide_system.txt",
            "guide_l1": "prompts/guide_l1.txt",
            "guide_qa": "prompts/guide_qa.txt",
            "guide_finalize": "prompts/guide_finalize.txt",
            "guide_summary": "prompts/guide_summary.txt",
        }
        self._prompts: dict[str, str] = {}
        for key, default_path in defaults.items():
            raw = prompt_cfg.get(key, default_path)
            self._prompts[key] = raw if os.path.isabs(raw) else os.path.join(self._config_dir, raw)

        # 加载系统提示词
        self._system_prompt = self._load_prompt("guide_system")
        self._l1_prompt = self._load_prompt("guide_l1")

        # 温度
        guide_cfg = cfg.get("guide_agent", {})
        self._l1_temp = l1_temperature or guide_cfg.get("l1_temperature", 0.1)
        # ENV 覆盖（方便调参）
        env_temp = os.getenv("L1_TEMPERATURE")
        if env_temp:
            try:
                self._l1_temp = float(env_temp)
            except ValueError:
                pass
        self._finalize_temp = finalize_temperature or guide_cfg.get("finalize_temperature", 0.1)

        # 短期记忆
        vol = short_memory_volume or guide_cfg.get("short_memory_volume", 100)
        self._short_memory = ShortMemoryBuffer(volume=vol)

        # 状态记录
        self._user_summary: str = ""               # 用户的一句话消费特征摘要（来自 DB）
        self._all_dialogues: list[dict] = []       # 完整对话记录
        self._tool_call_content: list[dict] = []   # 工具记录
        self._previous_analyses: list[str] = []    # 每轮 L1 分析结果追加，供后续轮次参考
        self._skill_filled_prompt: str =None
        self._product_prompt:list[dict]=[]
        self._l1_count: int = 0
        self._recommended_products: list[dict] = []
        self._product_followups: list[dict] = []

        # skill 加载状态（跨 analyze_l1 轮次持久化）
        self._skill_loaded: bool = False            # 品类 skill 是否已加载
        self._loaded_skill_context: str = ""        # 已加载的品类上下文（注入 skill_filled_prompt）
        self._products_loaded: bool = False          # 产品列表是否已加载

        # 加载产品 skill 提示词（通过 MCP find_product_prompt 动态获取）
        self._last_category: str = ""
        self._last_candidates: list[dict] = []

    # ================================================================
    # 会话生命周期
    # ================================================================

    def set_user_id(self, user_id: str):
        """设置当前用户 ID，加载该用户的画像"""
        self._user_id = user_id

    def start_session(self, session_id: str = "", context: str = "", user_id: str = ""):
        """开始新会话，加载历史用户画像"""
        self._session_id = session_id or f"sess_{int(time.time())}"
        self._user_id = user_id or ""
        # 从 MCP 加载历史画像（传入 user_id 按用户过滤）
        self.load_profile()
        # 可选：加载历史会话

        # 可选：加载历史会话
        if session_id:
            self.exec_tool("load_session", session_id=session_id)

        return self._session_id

    def end_session(self, context: str = "") -> None:
        """
        结束当前会话。
        1. 生成/更新用户摘要（基于原始历史数据，覆盖存储）
        2. 压缩短期记忆
        3. 通过 MCP 保存会话记录
        """
        # 1. 生成/更新用户摘要（new! 基于原始数据，而非摘要的摘要）
        if self._user_id:
            try:
                self.update_user_summary()
            except Exception as e:
                print(f"[end_session] 更新摘要失败: {e}", flush=True)

        # 2. 压缩短期记忆
        compact_result = self._short_memory.compact()

        # 3. MCP 保存会话
        dialogues_json = json.dumps(self._all_dialogues, ensure_ascii=False)
        self.exec_tool("save_session",
            session_id=self._session_id,
            context=context,
            dialogues=dialogues_json,
        )
        self._reset()

    # ================================================================
    # 用户画像
    # ================================================================

    def load_profile(self) -> str:
        """
        从 MCP 加载用户摘要（user_summary 类型，一句话消费特征）。
        存入 self._user_summary 供 analyze_l1 传给 LLM。
        """
        kwargs = build_kwargs_with_user_id(self._user_id, {"mem_type": "user_summary"})
        resp = self.exec_tool("load_user_profile", **kwargs)
        profiles = resp.get("profiles", []) if isinstance(resp, dict) else []
        if profiles:
            last = profiles[-1]
            if isinstance(last, dict):
                self._user_summary = json.dumps(last, ensure_ascii=False)
            elif isinstance(last, str):
                self._user_summary = last

        return self._user_summary

    # ================================================================
    # 用户摘要（每个用户一条，覆盖更新）
    # ================================================================

    def update_user_summary(self) -> dict:
        """
        基于当前会话数据重新生成用户的消费特征摘要。

        不依赖任何旧记忆（user_profile 已废弃），每次仅基于当前会话数据。
        未来可扩展为加载订单、评价等原始数据。
        覆盖存储为每个用户一条的纯文本摘要。
        """
        if not self._user_id:
            return {}

        # 组装 prompt（用原始对话记录替代已废弃的累积 profile）
        session_data = self._format_dialogues(self._all_dialogues) if self._all_dialogues else "（本轮会话无数据）"
        prompt = self._load_prompt("guide_summary").format(
            historical_records="（暂无历史数据）",
            session_data=session_data,
        )

        try:
            print(f"[agent] update_summary  prompt_len={len(prompt)}", flush=True)
            t0 = time.time()
            result_text = self.llm.chat(
                prompt,
                system_prompt=self._system_prompt,
                temperature=0.1,
            )
            result_text = result_text.strip().strip('"\'')
            t1 = time.time()
            print(f"[agent] update_summary ← {len(result_text)}B  {((t1-t0)*1000):.0f}ms", flush=True)
        except Exception as e:
            print(f"[update_user_summary] LLM 调用失败: {e}", flush=True)
            result_text = ""

        if not result_text:
            return {}

        # 3. 覆盖存储（固定 node_id 保证每个用户只存一条）
        node_id = f"summary_{self._user_id}"
        try:
            self.exec_tool("store_memory",
                node_type="user_summary",
                content=result_text,
                importance=0.9,
                tags=f"user:{self._user_id}",
                node_id=node_id,
            )
        except Exception as e:
            print(f"[update_user_summary] 存储失败: {e}", flush=True)

        return {"summary": result_text}

    def set_last_candidates(self, candidates: list[dict]):
        """存储最近推荐结果，供 check_product_exists 等后续查询"""
        self._last_candidates = candidates or []

    # ================================================================
    # L1 增量分析
    # ================================================================

    def analyze_l1(self, dialogues: list[dict]) -> dict:
        """
        每轮对话后调用：分析用户本轮需求，找出信息缺口并给出追问。

        LLM 可主动输出 tool 请求来加载品类 skill prompt：
          {"tool": true, "category": "笔记本电脑"}
        系统收到后调 MCP 加载追问 prompt，再重新调 LLM。

        dialogues: [{"role": "user"|"agent", "content": "..."}, ...]

        返回:
          {"reply": "追问文本", "intent": None}
        """
        self._all_dialogues.extend(dialogues)
        session_dialogues = self._format_dialogues(self._all_dialogues)

        #注入长期记忆（用户摘要+skill描述），以及短期记忆（用户会话+agent分析）
        prompt = self._l1_prompt.format(
            user_summary = self._user_summary or "（无）",
            product_skills = self._product_prompt,
            tool_call_content = self._tool_call_content,
            session_dialogues = session_dialogues,
            previous_analyses = self._previous_analyses
        )
        result = self._llm_call_with_tool_handling(prompt, session_dialogues)
        if result.get("reply") is not None:
            return {"reply": result["reply"]}
        elif result.get("final") is True:
            return self._handle_final(result.get("final_content", {}))

    def _handle_final(self, final_content: dict) -> dict:
        """
        LLM 认为信息已收集完毕 → 将 intent 持久化到 MCP，返回 final 信号。

        不调 product_agent，不搜索，不设 _recommended_products。
        """
        if not final_content:
            return {"final": True, "product_interacted": False}

        # 1. 持久化 intent 到 MCP
        self.exec_tool("store_memory",
            node_type="session_intent",
            content=json.dumps(final_content, ensure_ascii=False),
            importance=0.9,
            tags=f"session:{self._session_id}",
        )

        # 2. 保存导购阶段的完整会话（更新用户画像等）
        self.end_session(context="导购阶段完成，转入产品搜索")

        return {"final": True, "product_interacted": False}

    def _llm_call_with_tool_handling(self, prompt: str, session_dialogues: str, max_iterations: int = 3) -> dict:
        """
        调用 LLM 并处理工具调用循环
    
        Args:
            prompt: 初始提示词
            max_iterations: 最大迭代次数，防止死循环
    
        Returns:
            dict: 最终响应
        """
        iteration = 0
        current_prompt = prompt
        while iteration < max_iterations:
            iteration += 1
            result = self._llm_call(current_prompt)
            if result is None:
                result = self._llm_call(current_prompt+"请严格按照输出格式输出")
                if result is None:
                    raise ValueError("模型调用失败")
            analyse_text, params = result
            self._previous_analyses.append(analyse_text)

            if params.get("tool") is True:
                tool_name = params["tool_name"]
                tool_params = params["kwargs"]
                tool_call = self.exec_tool(tool_name=tool_name, tool_params=tool_params)
                if tool_call is None:
                    continue
                if tool_name == "find_product_prompt" and self._skill_filled_prompt is None:
                    self._skill_filled_prompt = tool_call.get("output", "")
                else:
                    self._tool_call_content.append(tool_call.get("output", ""))
                current_prompt = self._l1_prompt.format(
                    user_summary = self._user_summary or "（无）",
                    product_skills = self._product_prompt,
                    tool_call_content = self._tool_call_content,
                    session_dialogues = session_dialogues,
                    previous_analyses = self._previous_analyses
                )
            elif params.get("question") is True:
                return {"reply": params.get("question_content"), "intent": None}

            elif params.get("final") is True:
                return {"final": True, "final_content": params.get("final_content")}


            


    def _llm_call(self, prompt: str) -> tuple[str, dict[str, Any]] | None:
        """调用 LLM 返回原始文本"""
        try:
            print(f"[agent] L1 analyze  prompt_len={len(prompt)}  temp={self._l1_temp}", flush=True)
            t0 = time.time()
            orig_temp = self.llm.temperature
            self.llm.temperature = self._l1_temp
            raw_result = self.llm.chat(prompt, system_prompt=self._system_prompt).strip()
            self.llm.temperature = orig_temp
            t1 = time.time()
            print(f"[agent] L1 ← {len(raw_result)}B  {((t1-t0)*1000):.0f}ms", flush=True)
            result = self._parse_request(raw_result)
            return result
        except Exception as e:
            print(f"[agent] L1 ✗ {e}", flush=True)
            return None

    def _parse_request(self, raw: str) -> tuple[str, dict[str, Any]] | None:
        """
            解析输出请求有以下几种：
            [agent_analyse]:...........
            [json]:{"tool":true,"tool_name":"find_product_prompt", "kwargs": {"参数名1"：参数值，"参数名2":参数值}}
            
            [agent_analyse]:...........
            [json]:{"question":true,"question_content":"模型输出的问题"}
            
            [agent_analyse]:...........
            [json]:{"final":true,"final_content":{
                "category": "产品品类",
                "intent_delta": {
                "core_need": "一句话概括核心需求",
                "constraints": ["所有硬性要求"],
                "budget": {{"current": 0, "min": 0, "max": 0, "confidence": "high"}},
                "status": "confirmed"
                },
                "gaps": [],
                "follow_up_questions": []}
                }
            
            [agent_analyse]:...........
            [json]:{"promote":true,"promote_product":{"模型推荐的产品1":"理由","模型推荐的产品2":"理由"}}
        """
        if not raw:
            return None

        # 使用正则表达式匹配 [agent_analyse] 和 [json] 格式
        agent_analyse_pattern = r'\[agent_analyse\]:(.*?)(?=\n\[json\]|$)'
        json_pattern = r'\[json\]:(.*?)(?=\n\[agent_analyse\]|$)'
        parsed_data = None

        # 查找所有匹配
        agent_analyse_matches = re.findall(agent_analyse_pattern, raw, re.DOTALL)
        json_matches = re.findall(json_pattern, raw, re.DOTALL)

        # 如果没有 json 匹配，尝试直接解析整个 raw
        if not json_matches:
            parsed_data = _construct_fallback_json(raw)
        else:
        # 解析 JSON 数据
            
            for json_str in json_matches:
                try:
                    data = json.loads(json_str.strip())
                    if isinstance(data, dict):
                        parsed_data = data
                        break  # 取第一个有效的 JSON
                except json.JSONDecodeError as e:
                    print(f"JSON 解析错误: {e}")
                    continue

        if parsed_data is None:
            return None

        # 处理各种类型的请求
        # 1. tool 类型
        if parsed_data.get("tool") is True:
            tool_name = parsed_data.get("tool_name")
            kwargs = parsed_data.get("kwargs")
            if tool_name and kwargs:
                # 将 agent_analyse 内容合并到返回中
                return (
                    "\n".join(agent_analyse_matches).strip() if agent_analyse_matches else "",
                    {"tool_name": tool_name, "kwargs": kwargs}
                )
            return None

        # 2. question 类型
        if parsed_data.get("question") is True:
            question_content = parsed_data.get("question_content")
            if question_content:
                return (
                    "\n".join(agent_analyse_matches).strip() if agent_analyse_matches else "",
                    {"question": True, "question_content": question_content}
                )
            return None

        # 3. final 类型
        if parsed_data.get("final") is True:
            return (
                "\n".join(agent_analyse_matches).strip() if agent_analyse_matches else "",
                {"final": True, "final_content": parsed_data.get("final_content",{})}
            )

        # 4. promote 类型
        if parsed_data.get("promote") is True:
            promote_product = parsed_data.get("promote_product")
            if promote_product and isinstance(promote_product, dict):
                return (
                    "\n".join(agent_analyse_matches).strip() if agent_analyse_matches else "",
                    {"promote": True, "promote_product": promote_product}
                )
            return None
        return None



    # ================================================================
    # 产品交互记录
    # ================================================================

    def record_recommendation(self, candidates: list[dict], recommendations: list[dict] = None):
        """
        记录本次推荐的产品列表，供 session_info 持久化。

        candidates: search_by_guide_intent 返回的候选列表
        recommendations: LLM 推荐评分（可选），格式 [{product: {id, name, ...}, score, reasons}]
        """
        self._recommended_products = []
        seen = set()
        for c in (candidates or []):
            pid = str(c.get("id", ""))
            if not pid or pid in seen:
                continue
            seen.add(pid)
            entry = {
                "product_id": pid,
                "name": c.get("name", ""),
                "price": c.get("current_price", c.get("price", 0)),
                "brand": c.get("brand", ""),
                "rating": c.get("rating", 0),
                "url": c.get("url", ""),
            }
            # 合并 LLM 推荐评分
            if recommendations:
                for r in recommendations:
                    rp = r.get("product", {})
                    if str(rp.get("id", "")) == pid:
                        entry["score"] = r.get("score")
                        entry["reasons"] = r.get("reasons", [])
                        entry["caveats"] = r.get("caveats", [])
            self._recommended_products.append(entry)
        # 限制最多保留 5 条
        self._recommended_products = self._recommended_products[:5]

    def record_product_followup(self, product_id: str, question: str, answer: str = ""):
        """
        记录用户对某款产品的追问。

        自动从 _recommended_products 中查找产品名。
        调用时机：用户问"这款散热怎么样？"时，由外层服务调用。
        """
        product_name = ""
        for p in self._recommended_products:
            if p.get("product_id") == product_id:
                product_name = p.get("name", "")
                break

        self._product_followups.append({
            "product_id": product_id,
            "product_name": product_name,
            "question": question,
            "answer": answer,
            "asked_at": time.time(),
        })

    def exec_tool(self, tool_name: str, tool_params: dict | None = None, **kwargs) -> Any:
        """
        通过 MCP 客户端动态调用工具。

        收集所有可用 MCP 客户端（memory → product），逐个尝试调用。
        由 MCP 注册表决定工具属于哪个组，不再硬编码路由。

        Args:
            tool_name: MCP 工具名，需已注册到 MCP 服务端
            tool_params: 工具参数字典，与 kwargs 二选一
            kwargs: 关键字参数形式

        Returns:
            工具执行结果，失败返回 None
        """
        params = tool_params if tool_params is not None else kwargs
        if not isinstance(params, dict):
            raise ValueError(f"tool_params 必须为 dict，收到: {type(params).__name__}")

        clients = []
        if self._mcp is not None:
            clients.append(self._mcp)

        for mcp in clients:
            try:
                raw = mcp.call(tool_name, **params)
                result = json.loads(raw) if isinstance(raw, str) else raw
                if isinstance(result, dict) and "error" in result:
                    continue  # 不是该组的工具或不存在 → 试下一个
                return result
            except Exception:
                continue

        print(f"[exec_tool] {tool_name} 所有 MCP 客户端均不可用", flush=True)
        return None



    def _reset(self) -> None:
        """重置内部状态"""
        self._all_dialogues = []
        self._previous_analyses = []
        self._recommended_products = []
        self._product_followups = []
        self._last_category = ""
        self._skill_loaded = False
        self._loaded_skill_context = ""
        self._products_loaded = False
        self._l1_count = 0

    def remember(self, content: str, mem_type: str = "knowledge", importance: float = 0.5):
        """手动添加一条短期记忆"""
        node = MemoryNode.create(
            type=mem_type,
            source="user",
            content=content,
            importance=importance,
        )
        self._short_memory.add(node)

    def recall_short(self, query: str = "", limit: int = 10) -> list[MemoryNode]:
        """读取短期记忆"""
        memories = self._short_memory.read()
        if query:
            # 简单关键词过滤
            q = query.lower()
            memories = [m for m in memories if q in m.content.lower()]
        return memories[:limit]

    def _format_dialogues(self, dialogues: list[dict]) -> str:
        """格式化对话列表为文本"""
        lines = []
        for d in dialogues:
            role = d.get("role", "unknown")
            content = d.get("content", "")
            lines.append(f"[{role}]: {content}")
        return "\n".join(lines)

    def _load_prompt(self, key: str) -> str:
        """加载 prompt 模板"""
        path = self._prompts.get(key, "")
        if not path or not os.path.exists(path):
            if key == "guide_system":
                return "你是导购助手，请用中文分析用户购买需求。"
            raise FileNotFoundError(f"Prompt 文件不存在: key={key}, path={path}")
        return load_text_file(path, strict=True)


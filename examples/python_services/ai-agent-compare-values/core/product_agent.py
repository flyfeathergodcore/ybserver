"""
产品Agent — 商品搜索 + 推荐 + 答疑 + 对比

通过 MCP product 组工具调用京东联盟 API。
System prompt 通过 MCP Client 动态获取 skill 文档。
"""

import json
import os
import time
import logging
from typing import Any, Optional

from core.llm_client import LLMClient
from core.share_utils import load_text_file
from core.status import ProductState, ProductStateMachine

logger = logging.getLogger(__name__)


class ProductAgent:
    """产品Agent — 搜索→推荐→答疑→对比。

    构造时通过 MCP Client 动态获取 skill 文档拼接到 system prompt。
    """

    def __init__(
        self,
        llm: LLMClient,
        mcp: Any = None,
        config: Optional[dict] = None,
        search_temperature: float = 0.2,
        analysis_temperature: float = 0.3,
        default_platform: str = "jd",
        max_candidates: int = 10,
    ):
        self.llm = llm
        self._mcp = mcp

        cfg = config or {}
        product_cfg = cfg.get("product_agent", {})

        self._config_dir = os.path.dirname(os.path.abspath(
            os.getenv("CONFIG_PATH", "config.yaml")
        ))

        prompt_cfg = cfg.get("prompts", {})
        self._system_prompt_path = prompt_cfg.get(
            "product_system",
            os.path.join(self._config_dir, "prompts", "product_system.txt"),
        )
        self._system_prompt = self._build_system_prompt()

        self._search_temp = search_temperature or product_cfg.get("search_temperature", 0.2)
        self._analysis_temp = analysis_temperature or product_cfg.get("analysis_temperature", 0.3)
        self._default_platform = default_platform or product_cfg.get("default_platform", "jd")
        self._max_candidates = max_candidates or product_cfg.get("max_candidates", 10)

        self.statusmachine = ProductStateMachine()

        # 会话状态
        self._session_id = ""
        self._product_history: list[dict] = []
        self._current_intent: dict = {}
        self._current_candidates: list[dict] = []
        self._short_memory: list[Any] = []
        self._session_ready = False

        self._product_chat_prompt = self._load_file(os.path.join(
            self._config_dir, "prompts", "product_chat.txt"))

    # ================================================================
    # System Prompt 构建 — 通过 MCP Client 动态获取 skill 文档
    # ================================================================

    def _build_system_prompt(self) -> str:
        base = self._load_file(self._system_prompt_path)
        if not base:
            base = "你是商品分析助手，请用中文分析产品定位和推荐理由。"

        skill_context = ""
        if self._mcp:
            try:
                skill_context = self._mcp.get_skill_context()
            except Exception as e:
                logger.warning("获取 MCP skill 上下文失败: %s", e)

        if skill_context:
            return f"{base}\n\n# 详细工具文档\n\n{skill_context}"
        return base

    @staticmethod
    def _load_file(path: str) -> str:
        return load_text_file(path, default="", strict=False)

    # ================================================================
    # 入口方法（与 guide_agent.Run 一致）
    # ================================================================

    def start_session(self, session_id: str = ""):
        """启动会话，加载历史记录。"""
        self._session_id = session_id or f"prod_{int(time.time())}"
        self._session_ready = False
        self._product_history = []
        self._current_intent = {}
        self._current_candidates = []
        self._short_memory = [{"当前目标": "根据用户意图搜索和推荐产品"}]
        self.statusmachine.reset()
        if session_id:
            self._restore_product_history(session_id)
        return self._session_id

    def Run(self, session_message: dict) -> dict:
        """产品 Agent 入口 — 与 guide_agent.Run 同风格

        Args:
            session_message: {"role": "user"|"system", "content": "..."}

        Returns:
            {"reply": str, "candidates": list[dict]}
        """
        if session_message:
            self._product_history.append(session_message)

        self._prepare_session_runtime()

        if self.statusmachine.status.is_terminal:
            return {"reply": "当前会话已结束", "candidates": self._current_candidates}

        for _ in range(8):
            state = self.statusmachine.status
            prompt = self._build_prompt()

            reply, advanced = self._drive_state(state, prompt)
            if reply is not None:
                self._save_product_session()
                return {"reply": reply, "candidates": self._current_candidates}
            if advanced:
                continue

            if self.statusmachine.status.is_terminal:
                break

        self.statusmachine.go_to_FAILED()
        return {"reply": "当前状态无法处理请求，请检查状态机状态", "candidates": self._current_candidates}

    def end_session(self):
        """结束会话，持久化后清空状态。"""
        self._save_product_session()
        self._product_history = []
        self._current_candidates = []
        self._short_memory = []
        self._session_ready = False
        self.statusmachine.reset()

    def _prepare_session_runtime(self):
        """首次运行时连接 MCP、加载 intent。"""
        if self._session_ready:
            return

        # 连接 MCP 客户端（guide_agent 在 _prepare_session_runtime 也这样做）
        if self._mcp is not None:
            try:
                self._mcp.connect()
            except Exception as e:
                logger.warning("product MCP 连接失败: %s", e)

        if not self._current_intent:
            self._current_intent = self._load_intent_from_mcp(self._session_id)

        self._session_ready = True

    def _build_prompt(self) -> str:
        """构建 LLM prompt，包含候选商品、对话历史、MCP 工具列表、短期记忆。"""
        candidates_text = self._truncate_candidates_json(self._current_candidates)
        history_text = "\n".join(
            f"[{d['role']}]: {str(d.get('content', ''))[:500]}"
            for d in self._product_history[-10:]
        )

        mcp_tools = ""
        if self._mcp:
            try:
                tools = self._mcp.list_tools()
                mcp_tools = json.dumps(
                    [{"name": t.name, "params": t.inputSchema or {}} for t in tools],
                    ensure_ascii=False, indent=2)
            except Exception:
                mcp_tools = "（MCP 工具列表不可用）"

        prompt = self._product_chat_prompt.format(
            candidates=candidates_text,
            intent=json.dumps(self._current_intent, ensure_ascii=False, indent=2),
            mcp_tools=mcp_tools,
            short_memory=json.dumps(self._short_memory, ensure_ascii=False),
        )
        if history_text:
            prompt += f"\n\n## 当前对话\n{history_text}"
        return prompt

    def _drive_state(self, state, prompt) -> tuple[Optional[str], bool]:
        """状态驱动循环 — LLM 决定动作，状态机记录当前阶段。

        状态映射：
          search_category → SEARCHING → RESPONDING
          find_price(单商品) → DETAIL → RESPONDING
          find_price(多商品) → COMPARING → RESPONDING
          final → RECOMMENDING → DONE
          纯文本 → RESPONDING

        Returns:
            (reply_text, False)  → 直接回复，Run 返回
            (None, True)         → 继续循环
            (None, False)        → 失败退出
        """
        tool_map: dict[str, int] = {}
        for _ in range(3):
            result = self._consume_llm_result(prompt)
            if result is None:
                continue

            # 纯文本 = LLM 直接回复用户
            if isinstance(result, str):
                self._transition_to(ProductState.RESPONDING)
                return result, False

            # JSON 动作
            if result.get("tool") is True:
                tool_name = result.get("tool_name", "")
                kwargs = result.get("kwargs", {})
                if not tool_name or not isinstance(kwargs, dict):
                    continue

                tool_map[tool_name] = tool_map.get(tool_name, 0) + 1
                if tool_map[tool_name] > 2:
                    self._transition_to(ProductState.FAILED)
                    return "工具重复使用超限，任务退出", False

                # ── 根据工具类型切换状态 ──
                if tool_name == "search_category":
                    self._transition_to(ProductState.SEARCHING)
                elif tool_name == "find_price":
                    products = kwargs.get("products", [])
                    if isinstance(products, list) and len(products) > 1:
                        self._transition_to(ProductState.COMPARING)
                    else:
                        self._transition_to(ProductState.DETAIL)
                else:
                    self._transition_to(ProductState.RESPONDING)

                tool_resp = self._call_mcp(tool_name, **kwargs)
                self._product_history.append({
                    "role": "tool",
                    "content": json.dumps(tool_resp, ensure_ascii=False)[:1000],
                })
                self._short_memory.append({
                    "已执行工具": tool_name,
                    "返回结果": str(tool_resp).replace("\n", " ")[:200],
                })

                # ── 搜索后更新候选列表 ──
                if tool_name == "search_category":
                    if isinstance(tool_resp, list) and tool_resp:
                        enriched = self._enrich_details(tool_resp)
                        self._current_candidates = enriched[:self._max_candidates]
                        self._short_memory.append({
                            "更新候选": f"搜索到 {len(self._current_candidates)} 款产品",
                        })
                    self._transition_to(ProductState.RESPONDING)

                # ── 比价/详情后回到回复状态 ──
                if tool_name == "find_price":
                    self._transition_to(ProductState.RESPONDING)

                continue

            # Final — 推荐完成
            if result.get("final") is True:
                self._transition_to(ProductState.RECOMMENDING)
                self._transition_to(ProductState.DONE)
                self._short_memory.append({"当前目标": "推荐完毕"})
                return result.get("content", "已为您推荐完毕"), False

        return None, False

    def _transition_to(self, target: ProductState) -> bool:
        """安全的状态切换，失败时记录但不中断。"""
        current = self.statusmachine.status
        if not self.statusmachine.transition_to(target):
            logger.warning("状态切换失败: %s -> %s", current.value, target.value)
            return False
        return True

    def _consume_llm_result(self, prompt: str):
        """调用 LLM 并解析结果，失败时自动带严格提示重试。"""
        result = self._llm_call(prompt)
        if result is None:
            result = self._llm_call(prompt + "\n请严格按照输出格式输出")
        return result

    def _llm_call(self, prompt: str):
        """单次 LLM 调用 + 解析。"""
        try:
            print(f"[product] LLM call  prompt_len={len(prompt)}  temp={self._search_temp}", flush=True)
            t0 = time.time()
            original_temp = self.llm.temperature
            self.llm.temperature = self._search_temp
            raw = self.llm.chat(prompt, system_prompt=self._system_prompt).strip()
            self.llm.temperature = original_temp
            t1 = time.time()
            print(f"[product] LLM ← {len(raw)}B  {((t1 - t0) * 1000):.0f}ms", flush=True)
            return self._parse_request(raw)
        except Exception as e:
            print(f"[product] LLM ✗ {e}", flush=True)
            return None

    def _parse_request(self, raw: str):
        """解析 LLM 输出。

        - 含 [json]:{...} 且 tool/final 字段 → 返回 dict
        - 纯文本 → 返回 str（视为直接回复）
        - 空 → 返回 None
        """
        if not raw:
            return None
        from tools.json_parse import json_extract
        result = json_extract(raw)
        if result and isinstance(result, dict):
            if result.get("tool") is True or result.get("final") is True:
                return result
        return raw  # 纯文本视为直接回复

    # ================================================================
    # 搜索结果后处理
    # ================================================================

    def _enrich_details(self, candidates: list[dict]) -> list[dict]:
        """补全候选商品信息。

        search_category 已返回完整搜索结果（名称、好评率、类目等）。
        当前无需额外补全，直接返回。
        """
        return candidates

    # ================================================================
    # MCP 通用调用（与 guide_agent.exec_tool 对应）
    # ================================================================

    def _call_mcp(self, tool_name: str, **kwargs) -> dict:
        """通用 MCP 工具调用 — 类似 guide_agent.exec_tool。"""
        if self._mcp is None:
            logger.warning("[_call_mcp] product 组 MCP 不可用")
            return {}
        try:
            raw = self._mcp.call_tool(tool_name, **kwargs)
            result = json.loads(raw) if isinstance(raw, str) else raw
            if isinstance(result, dict) and "error" in result:
                logger.warning("[_call_mcp] %s 调用失败: %s", tool_name, result["error"])
                return {}
            return result
        except Exception as e:
            logger.warning("[_call_mcp] %s 异常: %s", tool_name, e)
            return {}

    # ================================================================
    # 会话持久化
    # ================================================================

    def _restore_product_history(self, session_id: str) -> bool:
        """从会话中恢复 product 阶段对话历史。"""
        session_resp = self._call_mcp("load_session", session_id=session_id)
        if not isinstance(session_resp, list) or not session_resp:
            return False
        for row in session_resp:
            if row.get("stage") != "product":
                continue
            content = row.get("content", [])
            if isinstance(content, str):
                content = json.loads(content)
            self._product_history = [
                {"role": m["role"], "content": m["content"]}
                for m in content if m.get("role") in ("user", "agent")
            ]
            return bool(self._product_history)
        return False

    def _load_intent_from_mcp(self, session_id: str) -> dict:
        """从新表 sessions 读取 guide 阶段存储的 intent。"""
        session_resp = self._call_mcp("load_session", session_id=session_id)
        if not isinstance(session_resp, list):
            return {}
        for row in session_resp:
            if row.get("stage") != "guide":
                continue
            content = row.get("content", [])
            if isinstance(content, str):
                try:
                    content = json.loads(content)
                except json.JSONDecodeError:
                    continue
            if not isinstance(content, list):
                continue
            for m in content:
                if m.get("message_type") == "final":
                    return m.get("metadata", {})
            break
        return {}

    def _save_product_session(self):
        """保存当前产品阶段状态。"""
        if not self._session_id:
            return
        messages = []
        for d in self._product_history[-30:]:
            msg_type = "tool_result" if d.get("role") == "tool" else "chat"
            messages.append({
                "role": d["role"], "phase": "product", "message_type": msg_type,
                "content": d["content"], "metadata": {},
            })
        self._call_mcp("save_session",
            session_id=self._session_id, stage="product", content=messages)

    # ================================================================
    # 工具方法
    # ================================================================

    @staticmethod
    def _truncate_candidates_json(candidates: list[dict], max_chars: int = 2000) -> str:
        """逐产品追加直到字符上限，避免 JSON 被从中截断。"""
        result = []
        for c in (candidates or []):
            chunk_str = json.dumps(c, ensure_ascii=False)
            total_so_far = sum(len(json.dumps(r, ensure_ascii=False)) for r in result)
            if total_so_far + len(chunk_str) > max_chars:
                break
            result.append(c)
        return json.dumps(result, ensure_ascii=False)[:max_chars]

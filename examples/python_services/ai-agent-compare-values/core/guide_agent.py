import json
import logging
import os
import re
import time
from typing import Any, Callable, Dict, Optional

from mcp_servers.client import MCPClient
from tools.json_parse import _construct_fallback_json

from core.llm_client import LLMClient
from core.share_utils import build_kwargs_with_user_id, load_text_file
from core.status import AgentState

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class StatueMachine:
    def __init__(self):
        self.status = AgentState.INIT
        self.previous_status = None
        self.context: dict[str, Any] = {}
        self.history: list[dict[str, Any]] = []
        self.step_count = 0
        self._on_enter_handlers: Dict[AgentState, Callable] = {}

    def reset(self):
        self.status = AgentState.INIT
        self.previous_status = None
        self.context = {}
        self.history = []
        self.step_count = 0

    def on_enter(self, state: AgentState, handler: Callable):
        self._on_enter_handlers[state] = handler

    def execute_current(self, **kwargs):
        handler = self._on_enter_handlers.get(self.status)
        if handler is None:
            logger.warning(f"⚠️ 当前状态未注册入口动作: {self.status.value}")
            return None

        try:
            ctx = {**self.context, **kwargs}
            result = handler(ctx)
            if result is not None:
                self.context[f"last_{self.status.value}_result"] = result
            return result
        except Exception as exc:
            logger.error(f"❌ 状态动作执行失败: {exc}")
            self.status = AgentState.FAILED
            return {"reply": "当前状态执行失败，请稍后再试", "advanced": False}

    def transition_to(self, target: AgentState, **kwargs) -> bool:
        if not self.status.can_go_to(target):
            logger.error(f"❌ 非法转移: {self.status.value} -> {target.value}")
            return False

        if self.step_count > self.status.max_retries and not target.is_terminal:
            logger.warning("⛔ 超过最大重试")
            return False

        old_status = self.status
        self.previous_status = old_status
        self.history.append(
            {
                "from": old_status.value,
                "to": target.value,
                "output": old_status.metadata.transition_context,
                "timestamp": time.time(),
                "step": self.step_count,
            }
        )

        self.status = target
        self.step_count = 0
        # kwargs 存入 context 供状态处理器使用
        self.context.update(kwargs)
        logger.info(f"✅ 转移: {old_status.value} -> {target.value} (步数: {self.step_count})")

        return True

    def go_to_DETAIL(self, **kwargs) -> bool:
        return self.transition_to(AgentState.DETAIL, **kwargs)

    def go_to_ASKING(self, **kwargs) -> bool:
        return self.transition_to(AgentState.ASKING, **kwargs)

    def go_to_DONE(self, **kwargs) -> bool:
        return self.transition_to(AgentState.DONE, **kwargs)

    def go_to_FAILED(self, **kwargs) -> bool:
        return self.transition_to(AgentState.FAILED, **kwargs)

    def go_to_OBSERVING(self, **kwargs) -> bool:
        return self.transition_to(AgentState.OBSERVING, **kwargs)


class ShoppingGuideAgent:
    """导购 Agent：负责需求识别、追问、工具调用和会话状态推进。"""

    def __init__(
        self,
        llm: LLMClient,
        config: Optional[dict] = None,
        l1_temperature: float = 0.1,
        finalize_temperature: float = 0.1,
        statue_machine: Optional[StatueMachine] = None,
    ):
        cfg = config or {}
        self.llm = llm

        # MCP 服务端地址（从配置读取端口，与 init_agents 启动的服务端对齐）
        mcp_cfg = cfg.get("mcp", {})
        mcp_port = mcp_cfg.get("port", 8765)
        self._mcp = MCPClient(server_url=f"http://localhost:{mcp_port}/sse", role="guide_agent")
        self._session_id = ""
        self._user_id = ""
        self._user_summary: str = ""
        self._all_dialogues: list[dict] = []
        self._event_log: list[dict[str, Any]] = []
        self._short_memory: list[Any] = []
        self._skill_list: str = ""
        self._skill_context: str = ""
        self._product_prompt: Any = None
        self._final_intent: dict[str, Any] = {}
        self._session_ready = False

        self._config_dir = os.path.dirname(os.path.abspath(os.getenv("CONFIG_PATH", "config.yaml")))
        prompt_cfg = cfg.get("prompts", {})
        print(prompt_cfg)
        defaults = {
            "guide_system": "prompts/guide_system.txt",
            "guide_l1": "prompts/guide_l1.txt",
        }
        self._prompts: dict[str, str] = {}
        for key, default_path in defaults.items():
            raw = prompt_cfg.get(key, default_path)
            self._prompts[key] = raw if os.path.isabs(raw) else os.path.join(self._config_dir, raw)

        self._system_prompt = self._load_prompt("guide_system")
        self._l1_prompt = self._load_prompt("guide_l1")

        guide_cfg = cfg.get("guide_agent", {})
        self._l1_temp = l1_temperature or guide_cfg.get("l1_temperature", 0.1)
        env_temp = os.getenv("L1_TEMPERATURE")
        if env_temp:
            try:
                self._l1_temp = float(env_temp)
            except ValueError:
                pass
        self._finalize_temp = finalize_temperature or guide_cfg.get("finalize_temperature", 0.1)

        self.statusmachine = statue_machine or StatueMachine()
        self._register_state_handlers()
        self._intent = {
            "category": "",
            "intent_delta": {
                "core_need": "",
                "constraints": [],
                "budget": {"current": 0, "min": 0, "max": 0, "confidence": "high"},
            },
        }

        try:
            self._skill_context = self.exec_tool("get_skill_context", role="guide") or ""
        except Exception:
            self._skill_context = ""

    def _register_state_handlers(self) -> None:
        self.statusmachine.on_enter(AgentState.INIT, self._on_enter_init)
        self.statusmachine.on_enter(AgentState.DETAIL, self._on_enter_detail)
        self.statusmachine.on_enter(AgentState.ASKING, self._on_enter_asking)
        self.statusmachine.on_enter(AgentState.OBSERVING, self._on_enter_observing)
        self.statusmachine.on_enter(AgentState.DONE, self._on_enter_done)
        self.statusmachine.on_enter(AgentState.FAILED, self._on_enter_failed)

    def _on_enter_init(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return self._run_state_loop(AgentState.INIT)

    def _on_enter_detail(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return self._run_state_loop(AgentState.DETAIL)

    def _on_enter_asking(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return self._run_state_loop(AgentState.ASKING)

    def _on_enter_observing(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return self._run_state_loop(AgentState.OBSERVING)

    def _on_enter_done(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return {"reply": "当前会话已结束", "advanced": False}

    def _on_enter_failed(self, _ctx: dict[str, Any]) -> dict[str, Any]:
        return {"reply": "当前状态无法处理请求，请检查状态机状态", "advanced": False}

    def _run_state_loop(self, state: AgentState) -> dict[str, Any]:
        session_dialogues = self._format_dialogues(self._all_dialogues)
        prompt = self._build_prompt(session_dialogues, self._product_prompt or "")
        reply, advanced = self._drive_state(state, prompt)
        return {"reply": reply, "advanced": advanced}

    def _append_event(
        self,
        event_type: str,
        role: str,
        content: Any,
        metadata: Optional[dict[str, Any]] = None,
        save_to_short_memory: bool = False,
    ) -> None:
        event = {
            "event_type": event_type,
            "role": role,
            "content": content,
            "metadata": metadata or {},
            "timestamp": time.time(),
        }
        self._event_log.append(event)

        if save_to_short_memory:
            self._short_memory.append(
                {
                    "event_type": event_type,
                    "role": role,
                    "content": content,
                    "metadata": metadata or {},
                }
            )

    def _prepare_session_runtime(self) -> None:
        if self._session_ready:
            return

        try:
            self._mcp.connect()
        except Exception as exc:
            logger.warning(f"连接 MCP 失败: {exc}")

        if not self._user_summary:
            self.load_profile()

        if not self._skill_list:
            skill_context = self._skill_context
            if not skill_context:
                try:
                    if hasattr(self._mcp, "get_skill_context"):
                        skill_context = self._mcp.get_skill_context()
                    else:
                        skill_context = self.exec_tool("get_skill_context", role="guide") or ""
                except Exception as exc:
                    logger.warning(f"加载 skill 上下文失败: {exc}")
                    skill_context = ""

            self._skill_list = skill_context if isinstance(skill_context, str) else json.dumps(skill_context, ensure_ascii=False)

        if not self._short_memory:
            self._short_memory.append(
                {"当前目标": "需要了解并确认用户需求哪些品类的产品，首次交互先完成打招呼和需求引导"}
            )

        self._session_ready = True

    def _build_prompt(self, session_dialogues: str, product_prompt: str = "") -> str:
        return self._l1_prompt.format(
            user_summary=self._user_summary or "（无）",
            skill_list=self._skill_list or self._skill_context or "（无）",
            product_prompt=product_prompt,
            session_dialogues=session_dialogues,
            short_memory=self._short_memory,
        )

    def _append_tool_memory(self, tool_name: str, tool_resp: Any) -> None:
        self._append_event(
            event_type="tool_result",
            role="tool",
            content=tool_resp,
            metadata={"tool_name": tool_name},
            save_to_short_memory=True,
        )

    def _update_intent(self, category: Any = None, intent_delta: Optional[dict] = None) -> None:
        if category:
            if isinstance(category, dict):
                self._intent.update(category)
            else:
                self._intent["category"] = category

        if isinstance(intent_delta, dict):
            current_delta = self._intent.setdefault(
                "intent_delta",
                {
                    "core_need": "",
                    "constraints": [],
                    "budget": {"current": 0, "min": 0, "max": 0, "confidence": "high"},
                },
            )
            current_delta.update(intent_delta)

    def _intent_ready(self) -> bool:
        intent_delta = self._intent.get("intent_delta") or {}
        return bool(
            self._intent.get("category")
            and intent_delta.get("core_need")
            and intent_delta.get("constraints")
            and intent_delta.get("budget")
        )

    def _consume_llm_result(self, prompt: str) -> tuple[str, dict[str, Any]] | None:
        result = self._llm_call(prompt)
        if result is None:
            result = self._llm_call(prompt + "请严格按照输出格式输出")
        return result

    def set_user_id(self, user_id: str):
        self._user_id = user_id

    def load_profile(self) -> str:
        if not self._user_id:
            return ""

        kwargs = build_kwargs_with_user_id(self._user_id, {"mem_type": "user_summary"})
        resp = self.exec_tool("load_user_profile", **kwargs)
        summary = ""
        if isinstance(resp, dict):
            profiles = resp.get("profiles", [])
            if profiles:
                last = profiles[-1]
                if isinstance(last, dict):
                    summary = json.dumps(last, ensure_ascii=False)
                elif isinstance(last, str):
                    summary = last
            elif isinstance(resp.get("summary"), str):
                summary = resp["summary"]
        elif isinstance(resp, str):
            summary = resp

        self._user_summary = summary
        return self._user_summary

    def start_session(self, session_id: str = "", context: str = "", user_id: str = ""):
        self._session_id = session_id or f"sess_{int(time.time())}"
        self._user_id = user_id or ""
        self._reset()
        self.statusmachine.reset()
        self._session_ready = False
        self.load_profile()

        if session_id:
            resp = self.exec_tool("load_session", session_id=session_id)
            if isinstance(resp, list) and resp:
                self._restore_from_session(resp)

        return self._session_id

    def _restore_from_session(self, session_data: list):
        if not session_data:
            return

        row = next((r for r in session_data if r.get("stage") == "guide"), session_data[0])
        messages = row.get("content", [])
        if isinstance(messages, str):
            messages = json.loads(messages)
        if not isinstance(messages, list):
            return

        for message in messages:
            role = message.get("role", "")
            content = message.get("content", "")
            msg_type = message.get("message_type", "")
            if msg_type != "event":
                continue

            event = content
            if isinstance(content, str):
                try:
                    event = json.loads(content)
                except json.JSONDecodeError:
                    continue
            if not isinstance(event, dict):
                continue

            self._event_log.append(event)
            event_type = event.get("event_type", "")
            event_role = event.get("role", role)
            event_content = event.get("content", "")
            event_metadata = event.get("metadata") or {}

            if event_type == "chat" and event_role in ("user", "agent"):
                self._all_dialogues.append({"role": event_role, "content": event_content})
            elif event_type == "tool_result":
                tool_name = event_metadata.get("tool_name", "")
                if tool_name == "find_product_prompt" and self._product_prompt is None:
                    self._product_prompt = event_content
            elif event_type in ("analysis", "intent", "final"):
                self._short_memory.append(
                    {
                        "event_type": event_type,
                        "role": event_role,
                        "content": event_content,
                        "metadata": event_metadata,
                    }
                )

            if event_type == "final" and isinstance(event_content, dict):
                self._final_intent = event_content

    def end_session(self, context: str = "", last_intent: str = "") -> None:
        messages = []
        for event in self._event_log:
            messages.append(
                {
                    "role": event.get("role", "system"),
                    "phase": "guide",
                    "message_type": "event",
                    "content": json.dumps(event, ensure_ascii=False),
                    "metadata": {},
                }
            )

        final_payload: dict[str, Any] = {}
        if last_intent:
            try:
                parsed = json.loads(last_intent)
                if isinstance(parsed, dict):
                    final_payload = parsed
            except json.JSONDecodeError:
                final_payload = {}
        if not final_payload and self._final_intent:
            final_payload = self._final_intent

        if final_payload:
            messages.append(
                {
                    "role": "system",
                    "phase": "guide",
                    "message_type": "final",
                    "content": "",
                    "metadata": final_payload,
                }
            )

        self.exec_tool(
            "save_session",
            session_id=self._session_id,
            stage="guide",
            content=messages,
        )
        self._reset()
        self.statusmachine.reset()
        self._session_ready = False

    def Run(self, session_message: dict) -> str:
        if session_message:
            self._all_dialogues.append(session_message)
            self._append_event(
                event_type="chat",
                role=session_message.get("role", "user"),
                content=session_message.get("content", ""),
                metadata={},
            )

        self._prepare_session_runtime()

        if self.statusmachine.status.is_terminal:
            return "当前会话已结束"

        for _ in range(8):
            try:
                result = self.statusmachine.execute_current(agent=self)
            except Exception:
                import traceback
                print("[guide] _drive_state 异常:", flush=True)
                traceback.print_exc()
                raise

            if not isinstance(result, dict):
                break

            reply = result.get("reply")
            advanced = bool(result.get("advanced"))
            if reply is not None:
                if not self.statusmachine.status.is_terminal and (
                    not self._all_dialogues
                    or self._all_dialogues[-1].get("role") != "agent"
                    or self._all_dialogues[-1].get("content") != reply
                ):
                    self._all_dialogues.append({"role": "agent", "content": reply})
                    self._append_event(
                        event_type="chat",
                        role="agent",
                        content=reply,
                        metadata={},
                    )
                return reply
            if advanced:
                continue

            if self.statusmachine.status.is_terminal:
                return "当前会话已结束"
            break

        self.statusmachine.go_to_FAILED()
        return "当前状态无法处理请求，请检查状态机状态"

    def _drive_state(self, state: AgentState, prompt: str) -> tuple[Optional[str], bool]:
        tool_map: dict[str, int] = {}
        for _ in range(state.max_retries):
            result = self._consume_llm_result(prompt)
            if result is None:
                continue

            analysis_text, payload = result
            if analysis_text:
                self._append_event(
                    event_type="analysis",
                    role="system",
                    content=analysis_text,
                    metadata={},
                    save_to_short_memory=True,
                )

            action_type = payload.get("type")
            if action_type == "tool":
                tool_name = payload["tool_name"]
                tool_map[tool_name] = tool_map.get(tool_name, 0) + 1
                if tool_map[tool_name] > 2:
                    self.statusmachine.go_to_FAILED()
                    return "重复使用工具，任务退出", False

                tool_resp = self.exec_tool(tool_name, payload.get("kwargs", {}))
                if tool_name == "find_product_prompt":
                    self._product_prompt = tool_resp
                    self._append_event(
                        event_type="tool_result",
                        role="tool",
                        content=tool_resp,
                        metadata={"tool_name": tool_name},
                        save_to_short_memory=True,
                    )
                else:
                    self._append_tool_memory(tool_name, tool_resp)
                continue

            if action_type == "intent":
                self._update_intent(payload.get("category"), payload.get("intent_delta"))
                self._append_event(
                    event_type="intent",
                    role="system",
                    content=payload,
                    metadata={},
                    save_to_short_memory=True,
                )
                if self._intent_ready():
                    if state in (AgentState.INIT, AgentState.DETAIL, AgentState.ASKING):
                        self.statusmachine.go_to_OBSERVING(payload=payload)
                    return None, True
                continue

            if action_type == "question":
                question = payload.get("question_content", "")
                if state in (AgentState.INIT, AgentState.DETAIL):
                    self.statusmachine.go_to_ASKING(payload=payload)
                elif state == AgentState.OBSERVING:
                    self.statusmachine.go_to_DETAIL(payload=payload)
                return question, False

            if action_type == "final":
                final_content = payload.get("final_content", {})
                if final_content:
                    self._final_intent = final_content if isinstance(final_content, dict) else {}
                    self._append_event(
                        event_type="final",
                        role="system",
                        content=final_content,
                        metadata={},
                        save_to_short_memory=True,
                    )

                self.statusmachine.go_to_DONE(payload=payload)
                self.end_session(
                    context="导购阶段完成，转入产品搜索",
                    last_intent=json.dumps(final_content, ensure_ascii=False) if final_content else "",
                )
                # end_session 内部会 reset 状态机，这里重新设为 DONE
                # 确保 grpc_server 的 _is_guide_done 能通过状态判断
                self.statusmachine.status = AgentState.DONE
                return "用户确认了意图，导购阶段完成，转入产品搜索阶段", False

            if payload.get("category") or payload.get("intent_delta"):
                self._update_intent(payload.get("category"), payload.get("intent_delta"))
                if self._intent_ready():
                    self.statusmachine.go_to_OBSERVING(payload=payload)
                    return None, True

        if state == AgentState.INIT:
            self.statusmachine.go_to_FAILED()
            return "初始化失败，暂不能提供服务，请稍后再试", False
        if state == AgentState.DETAIL:
            self.statusmachine.go_to_FAILED()
            return "长时间使用工具，导致 DETAIL 模式失败", False
        if state == AgentState.ASKING:
            return "当前仍在收集信息，请继续补充需求", False
        if state == AgentState.OBSERVING:
            self.statusmachine.go_to_FAILED()
            return "当前状态无法处理请求，请检查状态机状态", False
        return None, False

    def _handle_final(self, final_content: dict) -> dict:
        if not final_content:
            return {"final": True, "product_interacted": False}

        self.end_session(
            context="导购阶段完成，转入产品搜索",
            last_intent=json.dumps(final_content, ensure_ascii=False),
        )
        return {"final": True, "product_interacted": False}


    def _llm_call(self, prompt: str) -> tuple[str, dict[str, Any]] | None:
        try:
            print(f"[agent] L1 analyze  prompt_len={len(prompt)}  temp={self._l1_temp}", flush=True)
            t0 = time.time()
            original_temp = self.llm.temperature
            self.llm.temperature = self._l1_temp
            raw_result = self.llm.chat(prompt, system_prompt=self._system_prompt).strip()
            self.llm.temperature = original_temp
            t1 = time.time()
            print(f"[agent] L1 ← {len(raw_result)}B  {((t1 - t0) * 1000):.0f}ms", flush=True)
            result = self._parse_request(raw_result)
            print(f"[agent] L1 parsed result: {result}", flush=True)
            return result
        except Exception as exc:
            print(f"[agent] L1 ✗ {exc}", flush=True)
            return None

    def _parse_request(self, raw: str) -> tuple[str, dict[str, Any]] | None:
        if not raw:
            return None

        agent_analyse_pattern = r"\[agent_analyse\]:(.*?)(?=\n\[json\]|$)"
        json_pattern = r"\[json\]:(.*?)(?=\n\[agent_analyse\]|$)"

        agent_analyse_matches = re.findall(agent_analyse_pattern, raw, re.DOTALL)
        json_matches = re.findall(json_pattern, raw, re.DOTALL)

        parsed_data = None
        if not json_matches:
            parsed_data = _construct_fallback_json(raw)
        else:
            for json_str in json_matches:
                try:
                    data = json.loads(json_str.strip())
                    if isinstance(data, dict):
                        parsed_data = data
                        break
                except json.JSONDecodeError as exc:
                    print(f"JSON 解析错误: {exc}")

        if parsed_data is None:
            return None

        analysis_text = "\n".join(agent_analyse_matches).strip() if agent_analyse_matches else ""

        if parsed_data.get("tool") is True:
            tool_name = parsed_data.get("tool_name")
            kwargs = parsed_data.get("kwargs")
            if tool_name and kwargs:
                return analysis_text, {"type": "tool", "tool_name": tool_name, "kwargs": kwargs}
            return None

        if parsed_data.get("question") is True:
            question_content = parsed_data.get("question_content")
            if question_content:
                return analysis_text, {"type": "question", "question_content": question_content}
            return None

        if parsed_data.get("final") is True:
            return analysis_text, {"type": "final", "final_content": parsed_data.get("final_content", {})}

        if parsed_data.get("category") or parsed_data.get("intent_delta"):
            payload = {"type": "intent"}
            if parsed_data.get("category"):
                payload["category"] = parsed_data.get("category")
            if parsed_data.get("intent_delta"):
                payload["intent_delta"] = parsed_data.get("intent_delta")
            return analysis_text, payload

        return None

    def exec_tool(self, tool_name: str, tool_params: dict | None = None, **kwargs) -> Any:
        params = tool_params if tool_params is not None else kwargs
        if not isinstance(params, dict):
            raise ValueError(f"tool_params 必须为 dict，收到: {type(params).__name__}")

        clients = []
        if self._mcp is not None:
            clients.append(self._mcp)

        for mcp in clients:
            try:
                raw = mcp.call_tool(tool_name, **params)
                result = json.loads(raw) if isinstance(raw, str) else raw
                if isinstance(result, dict) and "error" in result:
                    continue
                return result
            except Exception:
                continue

        print(f"[exec_tool] {tool_name} 所有 MCP 客户端均不可用", flush=True)
        return None

    def _reset(self) -> None:
        self._all_dialogues = []
        self._event_log = []
        self._short_memory = []
        self._product_prompt = None
        self._final_intent = {}

    def _format_dialogues(self, dialogues: list[dict]) -> str:
        lines = []
        for dialogue in dialogues:
            role = dialogue.get("role", "unknown")
            content = dialogue.get("content", "")
            lines.append(f"[{role}]: {content}")
        return "\n".join(lines)

    def _load_prompt(self, key: str) -> str:
        path = self._prompts.get(key, "")
        if not path or not os.path.exists(path):
            if key == "guide_system":
                return "你是导购助手，请用中文分析用户购买需求。"
            raise FileNotFoundError(f"Prompt 文件不存在: key={key}, path={path}")
        return load_text_file(path, strict=True)
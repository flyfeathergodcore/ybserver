import logging
import time
from enum import Enum
from dataclasses import dataclass
from typing import Any, Callable, Dict, Set, Optional

logger = logging.getLogger(__name__)

# 1. 元数据定义（不变）
@dataclass(frozen=True)
class StateMetadata:
    timeout_seconds: int = 30
    max_retries: int = 3
    description: str = ""
    transition_context:str = ""
    requires_user_input: bool = False
    is_terminal: bool = False  # 新增：是否终止状态

# 2. AgentState — 枚举值 + 属性，元数据在类外注入避免 EnumMeta 干扰
class AgentState(Enum):
    INIT = "init"
    OBSERVING= "observing"
    DETAIL = "detail"
    ASKING = "asking"
    DONE = "done"
    FAILED = "failed"

    # ===== 辅助属性 =====
    @property
    def metadata(self) -> StateMetadata:
        return _agent_metadata[self]

    @property
    def timeout(self) -> int:
        return self.metadata.timeout_seconds

    @property
    def max_retries(self) -> int:
        return self.metadata.max_retries

    @property
    def description(self) -> str:
        return self.metadata.description

    @property
    def is_terminal(self) -> bool:
        return self.metadata.is_terminal

    @property
    def is_active(self) -> bool:
        return not self.is_terminal

    # ===== 转移校验方法 =====
    @classmethod
    def can_transition(cls, from_state: "AgentState", to_state: "AgentState") -> bool:
        """检查是否能从 from_state 转移到 to_state"""
        if from_state is None or to_state is None:
            return False
        if from_state.is_terminal:
            return False
        allowed = _agent_transitions.get(from_state, set())
        return to_state in allowed

    @classmethod
    def get_allowed_transitions(cls, from_state: "AgentState") -> Set["AgentState"]:
        """获取从某状态可以转移到的所有状态"""
        if from_state is None or from_state.is_terminal:
            return set()
        return _agent_transitions.get(from_state, set())

    # ===== 便捷方法：直接在当前实例上调用 =====
    def can_go_to(self, target: "AgentState") -> bool:
        """实例方法：self 是否能转移到 target"""
        return self.can_transition(self, target)

    def allowed_targets(self) -> Set["AgentState"]:
        """实例方法：获取当前状态允许转移的目标列表"""
        return self.get_allowed_transitions(self)

    def set_transition_context(self,context:str)->bool:
        if context:
            self.metadata.transition_context = context
            return True
        return False


# ===== AgentState 元数据 & 转移表（类外定义，避免 Python 3.13 EnumMeta 将其变为枚举成员）=====

_agent_metadata: Dict[AgentState, StateMetadata] = {
    AgentState.INIT: StateMetadata(
        timeout_seconds=15,
        description="初始化阶段",
        max_retries=2
    ),
    AgentState.DETAIL: StateMetadata(
        timeout_seconds=5,
        max_retries=2,
        description="探索细节部分",
    ),
    AgentState.ASKING: StateMetadata(
        timeout_seconds=30,
        max_retries=3,
        description="询问更多信息"
    ),
    AgentState.OBSERVING: StateMetadata(
        timeout_seconds=10,
        max_retries=1,
        description="结果观察阶段"
    ),
    AgentState.DONE: StateMetadata(
        timeout_seconds=0,
        description="完成状态",
        is_terminal=True
    ),
    AgentState.FAILED: StateMetadata(
        timeout_seconds=0,
        description="失败状态",
        is_terminal=True
    ),
}

_agent_transitions: Dict[AgentState, Set[AgentState]] = {
    AgentState.INIT: {AgentState.DETAIL, AgentState.ASKING, AgentState.OBSERVING},
    AgentState.DETAIL: {AgentState.ASKING, AgentState.OBSERVING, AgentState.FAILED},
    AgentState.ASKING: {AgentState.OBSERVING, AgentState.FAILED},
    AgentState.OBSERVING: {AgentState.DETAIL, AgentState.DONE, AgentState.FAILED},
    AgentState.DONE: set(),
    AgentState.FAILED: set(),
}


# ============================================================
# ProductState — 产品 Agent 状态枚举
# ============================================================

class ProductState(Enum):
    """产品 Agent 状态"""
    INIT = "init"
    SEARCHING = "searching"
    RESPONDING = "responding"
    DETAIL = "detail"
    COMPARING = "comparing"
    RECOMMENDING = "recommending"
    DONE = "done"
    FAILED = "failed"

    @property
    def is_terminal(self) -> bool:
        return self in (ProductState.DONE, ProductState.FAILED)


# ============================================================
# ProductStateMachine — 产品 Agent 状态机
# ============================================================

class ProductStateMachine:
    """产品 Agent 状态机"""

    _TRANSITIONS: dict[ProductState, Set[ProductState]] = {
        ProductState.INIT: {
            ProductState.SEARCHING,
            ProductState.RESPONDING,
            ProductState.FAILED,
        },
        ProductState.SEARCHING: {
            ProductState.RESPONDING,
            ProductState.FAILED,
        },
        ProductState.RESPONDING: {
            ProductState.SEARCHING,
            ProductState.DETAIL,
            ProductState.COMPARING,
            ProductState.RECOMMENDING,
            ProductState.DONE,
            ProductState.FAILED,
        },
        ProductState.DETAIL: {
            ProductState.RESPONDING,
            ProductState.FAILED,
        },
        ProductState.COMPARING: {
            ProductState.RESPONDING,
            ProductState.FAILED,
        },
        ProductState.RECOMMENDING: {
            ProductState.RESPONDING,
            ProductState.DONE,
            ProductState.FAILED,
        },
        ProductState.DONE: set(),
        ProductState.FAILED: set(),
    }

    def __init__(self):
        self.status = ProductState.INIT
        self.previous_status: Optional[ProductState] = None
        self.context: dict[str, Any] = {}
        self.history: list[dict[str, Any]] = []
        self.step_count = 0
        self._on_enter_handlers: dict[ProductState, Callable] = {}

    def reset(self):
        self.status = ProductState.INIT
        self.previous_status = None
        self.context = {}
        self.history = []
        self.step_count = 0

    def on_enter(self, state: ProductState, handler: Callable):
        self._on_enter_handlers[state] = handler

    def transition_to(self, target: ProductState, **kwargs) -> bool:
        allowed = self._TRANSITIONS.get(self.status, set())
        if target not in allowed:
            logger.error("❌ 产品状态非法转移: %s -> %s", self.status.value, target.value)
            return False

        old_status = self.status
        self.previous_status = old_status
        self.history.append(
            {
                "from": old_status.value,
                "to": target.value,
                "timestamp": time.time(),
                "step": self.step_count,
            }
        )

        self.status = target
        self.step_count += 1
        logger.info("✅ 产品状态转移: %s -> %s", old_status.value, target.value)

        if target in self._on_enter_handlers:
            try:
                ctx = {**self.context, **kwargs}
                result = self._on_enter_handlers[target](ctx)
                if result is not None:
                    self.context[f"last_{target.value}_result"] = result
            except Exception as exc:
                logger.error("❌ 产品状态入口执行失败: %s", exc)
                self.status = ProductState.FAILED
                return False

        return True

    def go_to_SEARCHING(self, **kwargs) -> bool:
        return self.transition_to(ProductState.SEARCHING, **kwargs)

    def go_to_RESPONDING(self, **kwargs) -> bool:
        return self.transition_to(ProductState.RESPONDING, **kwargs)

    def go_to_DETAIL(self, **kwargs) -> bool:
        return self.transition_to(ProductState.DETAIL, **kwargs)

    def go_to_COMPARING(self, **kwargs) -> bool:
        return self.transition_to(ProductState.COMPARING, **kwargs)

    def go_to_RECOMMENDING(self, **kwargs) -> bool:
        return self.transition_to(ProductState.RECOMMENDING, **kwargs)

    def go_to_DONE(self, **kwargs) -> bool:
        return self.transition_to(ProductState.DONE, **kwargs)

    def go_to_FAILED(self, **kwargs) -> bool:
        return self.transition_to(ProductState.FAILED, **kwargs)

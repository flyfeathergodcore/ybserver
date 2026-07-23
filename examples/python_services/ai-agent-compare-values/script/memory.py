"""
Memory module for storing conversation history, plan state, and execution results.
"""

from dataclasses import dataclass, field
from typing import Any
from datetime import datetime


@dataclass
class Message:
    """Represents a single message in the conversation."""
    role: str  # "user", "assistant", "system", "tool"
    content: str
    metadata: dict = field(default_factory=dict)
    timestamp: str = field(default_factory=lambda: datetime.now().isoformat())


@dataclass
class PlanStep:
    """Represents a single step in the agent's plan."""
    id: str
    description: str
    status: str = "pending"  # "pending", "in_progress", "completed", "failed"
    result: str = ""
    error: str = ""
    started_at: str = ""
    completed_at: str = ""


class Memory:
    """
    Manages conversation history, plan state, and execution context.
    Provides a clean interface for the agent to read/write state.
    """

    def __init__(self, max_messages: int = 100):
        self.max_messages = max_messages
        self.messages: list[Message] = []
        self.plan_steps: list[PlanStep] = []
        self.context: dict[str, Any] = {}  # arbitrary key-value store

    # --- Message management ---

    def add_message(self, role: str, content: str, metadata: dict | None = None) -> Message:
        msg = Message(role=role, content=content, metadata=metadata or {})
        self.messages.append(msg)
        # Trim old messages if exceeding limit
        if len(self.messages) > self.max_messages:
            self.messages = self.messages[-self.max_messages:]
        return msg

    def get_messages(self, role: str | None = None) -> list[Message]:
        if role:
            return [m for m in self.messages if m.role == role]
        return list(self.messages)

    def get_conversation_history(self, last_n: int | None = None) -> list[Message]:
        if last_n:
            return self.messages[-last_n:]
        return list(self.messages)

    # --- Plan management ---

    def add_plan_step(self, step_id: str, description: str) -> PlanStep:
        step = PlanStep(id=step_id, description=description)
        self.plan_steps.append(step)
        return step

    def update_step_status(self, step_id: str, status: str, result: str = "", error: str = "") -> PlanStep | None:
        for step in self.plan_steps:
            if step.id == step_id:
                step.status = status
                if result:
                    step.result = result
                if error:
                    step.error = error
                return step
        return None

    def get_pending_steps(self) -> list[PlanStep]:
        return [s for s in self.plan_steps if s.status in ("pending", "in_progress")]

    def get_completed_steps(self) -> list[PlanStep]:
        return [s for s in self.plan_steps if s.status == "completed"]

    def get_failed_steps(self) -> list[PlanStep]:
        return [s for s in self.plan_steps if s.status == "failed"]

    def get_all_steps(self) -> list[PlanStep]:
        return list(self.plan_steps)

    # --- Context management ---

    def set_context(self, key: str, value: Any) -> None:
        self.context[key] = value

    def get_context(self, key: str, default: Any = None) -> Any:
        return self.context.get(key, default)

    def get_all_context(self) -> dict[str, Any]:
        return dict(self.context)

    def clear(self) -> None:
        """Reset all state."""
        self.messages.clear()
        self.plan_steps.clear()
        self.context.clear()

    def summary(self) -> str:
        """Return a human-readable summary of current state."""
        lines = [
            f"Messages: {len(self.messages)}",
            f"Plan Steps: {len(self.plan_steps)} "
            f"({sum(1 for s in self.plan_steps if s.status == 'completed')} completed, "
            f"{sum(1 for s in self.plan_steps if s.status == 'failed')} failed, "
            f"{sum(1 for s in self.plan_steps if s.status in ('pending', 'in_progress'))} pending)",
        ]
        return "\n".join(lines)

"""
Tool system for the agent. Defines a base tool interface and a registry for managing tools.
"""

import inspect
from abc import ABC, abstractmethod
from typing import Any, Callable


class BaseTool(ABC):
    """
    Abstract base class for all agent tools.
    Subclasses must implement name, description, and execute.
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """Unique identifier for the tool."""
        ...

    @property
    @abstractmethod
    def description(self) -> str:
        """Human-readable description of what the tool does."""
        ...

    @property
    def parameters(self) -> dict[str, Any]:
        """
        JSON Schema-like description of tool parameters.
        Override to provide structured parameter info.
        """
        sig = inspect.signature(self.execute)
        params = {}
        for name, param in sig.parameters.items():
            if name == "self":
                continue
            params[name] = {
                "type": self._param_type(param.annotation),
                "description": param.default if param.default is not inspect.Parameter.empty else "(required)",
            }
        return params

    @abstractmethod
    def execute(self, **kwargs) -> Any:
        """Execute the tool with given arguments."""
        ...

    @staticmethod
    def _param_type(annotation) -> str:
        mapping = {
            str: "string",
            int: "integer",
            float: "number",
            bool: "boolean",
            list: "array",
            dict: "object",
        }
        return mapping.get(annotation, "string")

    def to_dict(self) -> dict:
        """Serialize tool info for LLM prompt generation."""
        return {
            "name": self.name,
            "description": self.description,
            "parameters": self.parameters,
        }


class ToolRegistry:
    """
    Registry for managing available tools.
    The agent queries this registry to know what tools are available.
    """

    def __init__(self):
        self._tools: dict[str, BaseTool] = {}

    def register(self, tool: BaseTool) -> None:
        """Register a tool by instance."""
        self._tools[tool.name] = tool

    def register_function(self, func: Callable, name: str | None = None, description: str = "") -> None:
        """
        Register a plain function as a tool.
        Automatically wraps it in a DynamicTool.
        """
        tool_name = name or func.__name__
        tool = DynamicTool(func=func, name=tool_name, description=description)
        self._tools[tool_name] = tool

    def unregister(self, name: str) -> bool:
        """Remove a tool by name."""
        if name in self._tools:
            del self._tools[name]
            return True
        return False

    def get(self, name: str) -> BaseTool | None:
        return self._tools.get(name)

    def list_tools(self) -> list[dict]:
        """Return serialized info for all registered tools."""
        return [t.to_dict() for t in self._tools.values()]

    def get_tool_names(self) -> list[str]:
        return list(self._tools.keys())

    def execute_tool(self, tool_name: str, **kwargs) -> Any:
        """Find and execute a tool by name."""
        tool = self._tools.get(tool_name)
        if not tool:
            raise ValueError(f"Tool '{tool_name}' not found. Available: {self.get_tool_names()}")
        return tool.execute(**kwargs)

    def __len__(self) -> int:
        return len(self._tools)

    def __contains__(self, name: str) -> bool:
        return name in self._tools


class DynamicTool(BaseTool):
    """
    A tool created from a plain function. Useful for quick tool registration.
    """

    def __init__(self, func: Callable, name: str, description: str = ""):
        self._func = func
        self._name = name
        self._description = description or func.__doc__ or ""

    @property
    def name(self) -> str:
        return self._name

    @property
    def description(self) -> str:
        return self._description

    def execute(self, **kwargs) -> Any:
        return self._func(**kwargs)

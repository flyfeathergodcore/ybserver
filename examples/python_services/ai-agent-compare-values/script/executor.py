"""
Executor module: executes plan steps by invoking tools.
"""

from typing import Any
from memory import Memory, PlanStep
from tools import ToolRegistry


class Executor:
    """
    Executes plan steps by invoking registered tools.
    Handles step lifecycle: pending -> in_progress -> completed/failed.
    """

    def __init__(self, tool_registry: ToolRegistry):
        self.tool_registry = tool_registry

    def execute_step(self, step: PlanStep, memory: Memory) -> PlanStep:
        """
        Execute a single plan step.

        Args:
            step: The PlanStep to execute.
            memory: Current memory state.

        Returns:
            The updated PlanStep with result or error.
        """
        memory.update_step_status(step.id, "in_progress")

        try:
            # Extract tool name and arguments from step description
            tool_name, tool_args = self._parse_step(step)
            result = self.tool_registry.execute_tool(tool_name, **tool_args)
            memory.update_step_status(step.id, "completed", result=str(result))
            return step
        except Exception as e:
            memory.update_step_status(step.id, "failed", error=str(e))
            return step

    def execute_all(self, memory: Memory) -> list[PlanStep]:
        """
        Execute all pending steps sequentially.

        Args:
            memory: Memory containing plan steps.

        Returns:
            List of all executed PlanSteps.
        """
        steps = memory.get_pending_steps()
        results = []
        for step in steps:
            result = self.execute_step(step, memory)
            results.append(result)
        return results

    def _parse_step(self, step: PlanStep) -> tuple[str, dict]:
        """
        Parse a step's description into (tool_name, tool_args).

        Expected format: "tool_name: arg1=value1, arg2=value2"
        Or just: "tool_name"
        """
        import re
        # Try to parse "tool_name: args" format
        match = re.match(r'^(\w+):\s*(.*)', step.description)
        if match:
            tool_name = match.group(1)
            args_str = match.group(2).strip()
            args = self._parse_args(args_str)
            return tool_name, args

        # Fallback: treat entire description as tool name with no args
        return step.description.strip(), {}

    @staticmethod
    def _parse_args(args_str: str) -> dict:
        """Parse 'key=value, key2=value2' string into a dict."""
        if not args_str:
            return {}
        args = {}
        for pair in args_str.split(','):
            pair = pair.strip()
            if '=' in pair:
                key, value = pair.split('=', 1)
                args[key.strip()] = Executor._try_parse_value(value.strip())
            else:
                args[pair] = True
        return args

    @staticmethod
    def _try_parse_value(value: str) -> Any:
        """Try to convert string value to appropriate Python type."""
        if value.lower() in ('true', 'yes'):
            return True
        if value.lower() in ('false', 'no'):
            return False
        if value.lower() in ('none', 'null'):
            return None
        try:
            return int(value)
        except ValueError:
            pass
        try:
            return float(value)
        except ValueError:
            pass
        return value

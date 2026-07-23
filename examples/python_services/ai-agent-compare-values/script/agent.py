"""
Main Agent class: orchestrates the Plan/Action loop.

Usage:
    agent = Agent(llm_client=my_llm, tools=[MyTool(), AnotherTool()])
    result = agent.run("Your goal here")
"""

from typing import Any
from memory import Memory
from planner import Planner
from executor import Executor
from tools import ToolRegistry, BaseTool


class Agent:
    """
    A standard Plan/Action agent that:
    1. Plans: breaks user goal into steps
    2. Acts: executes each step using tools
    3. Observes: checks results and replans if needed
    4. Loops: continues until goal is achieved or max iterations reached
    """

    def __init__(
        self,
        llm_client: Any | None = None,
        tools: list[BaseTool] | None = None,
        max_iterations: int = 10,
        verbose: bool = False,
    ):
        """
        Args:
            llm_client: Optional LLM client for planning and response generation.
            tools: Optional list of tools to register.
            max_iterations: Maximum number of plan-execute cycles.
            verbose: If True, print detailed execution logs.
        """
        self.memory = Memory()
        self.tool_registry = ToolRegistry()
        self.planner = Planner(llm_client=llm_client)
        self.executor = Executor(tool_registry=self.tool_registry)
        self.max_iterations = max_iterations
        self.verbose = verbose

        # Register provided tools
        if tools:
            for tool in tools:
                self.tool_registry.register(tool)

        # Register default tools
        self._register_default_tools()

    # --- Core API ---

    def run(self, goal: str) -> dict:
        """
        Execute the Plan/Action loop for a given goal.

        Args:
            goal: The user's high-level goal.

        Returns:
            A dict with 'success', 'steps', 'output', and 'iterations'.
        """
        self.memory.clear()
        self.memory.add_message("user", goal)

        final_output = ""
        iterations = 0

        for i in range(self.max_iterations):
            iterations += 1
            if self.verbose:
                print(f"\n=== Iteration {i + 1} ===")

            # Step 1: Plan
            plan_steps = self.planner.plan(goal, self.memory)
            if self.verbose:
                print(f"  Plan: {len(plan_steps)} steps")
                for s in plan_steps:
                    print(f"    - [{s.id}] {s.description}")

            # Step 2: Execute
            executed_steps = self.executor.execute_all(self.memory)

            # Step 3: Check completion
            completed = self.memory.get_completed_steps()
            failed = self.memory.get_failed_steps()
            pending = self.memory.get_pending_steps()

            if self.verbose:
                print(f"  Results: {len(completed)} completed, {len(failed)} failed, {len(pending)} pending")

            # Collect outputs from completed steps
            for step in completed:
                if step.result:
                    final_output += step.result + "\n"

            # Check if done
            if not pending:
                break

            # If there are failures, add to memory for replanning
            for step in failed:
                self.memory.add_message(
                    "assistant",
                    f"Step {step.id} failed: {step.error}",
                )

            # Replan with updated context (LLM-driven agents can adapt)
            if self.verbose:
                print(f"  Replanning with updated context...")

        # Final result
        success = len(self.memory.get_failed_steps()) == 0
        return {
            "success": success,
            "iterations": iterations,
            "steps": self.memory.get_all_steps(),
            "output": final_output.strip(),
        }

    def run_step_by_step(self, goal: str) -> dict:
        """
        Execute one iteration of the Plan/Action loop.
        Useful for debugging or interactive use.
        """
        plan_steps = self.planner.plan(goal, self.memory)
        executed_steps = self.executor.execute_all(self.memory)
        return {
            "steps": executed_steps,
            "pending": len(self.memory.get_pending_steps()),
        }

    # --- Tool management ---

    def add_tool(self, tool: BaseTool) -> None:
        """Add a tool to the agent."""
        self.tool_registry.register(tool)

    def remove_tool(self, name: str) -> bool:
        """Remove a tool by name."""
        return self.tool_registry.unregister(name)

    def list_tools(self) -> list[str]:
        """List all registered tool names."""
        return self.tool_registry.get_tool_names()

    # --- Internal ---

    def _register_default_tools(self) -> None:
        """Register built-in default tools."""
        from default_tools import (
            ReadFileTool,
            WriteFileTool,
            SearchTool,
            CalculatorTool,
        )
        self.tool_registry.register(ReadFileTool())
        self.tool_registry.register(WriteFileTool())
        self.tool_registry.register(SearchTool())
        self.tool_registry.register(CalculatorTool())

    # --- Properties ---

    @property
    def tools(self) -> ToolRegistry:
        return self.tool_registry

    @property
    def memory(self) -> Memory:
        return self._memory

    @memory.setter
    def memory(self, value: Memory) -> None:
        self._memory = value

    def __repr__(self) -> str:
        return (
            f"Agent(tools={self.list_tools()}, "
            f"max_iterations={self.max_iterations}, "
            f"verbose={self.verbose})"
        )

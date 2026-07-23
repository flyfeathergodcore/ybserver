"""
Planner module: breaks user goals into executable steps.
"""

from typing import Any
from memory import Memory, PlanStep


class Planner:
    """
    Responsible for decomposing a high-level goal into a sequence of plan steps.
    Can use an LLM or a rule-based approach.
    """

    def __init__(self, llm_client: Any | None = None):
        """
        Args:
            llm_client: Optional LLM client for AI-driven planning.
                         If None, falls back to simple rule-based planning.
        """
        self.llm_client = llm_client

    def plan(self, goal: str, memory: Memory) -> list[PlanStep]:
        """
        Generate a plan (list of PlanStep) from a user goal.

        Args:
            goal: The user's high-level goal.
            memory: Current memory state (for context).

        Returns:
            A list of PlanStep objects.
        """
        if self.llm_client:
            return self._llm_plan(goal, memory)
        return self._rule_based_plan(goal, memory)

    def _llm_plan(self, goal: str, memory: Memory) -> list[PlanStep]:
        """Use LLM to generate structured plan steps."""
        prompt = self._build_plan_prompt(goal, memory)
        response = self.llm_client.generate(prompt)
        return self._parse_plan_response(response, memory)

    def _rule_based_plan(self, goal: str, memory: Memory) -> list[PlanStep]:
        """
        Simple rule-based planner.
        Splits the goal by common action keywords.
        """
        import re
        actions = re.split(r'\b(?:and|then|also|next|also)\b', goal, flags=re.IGNORECASE)
        steps = []
        for i, action in enumerate(actions, 1):
            action = action.strip()
            if action:
                step = memory.add_plan_step(
                    step_id=f"step_{i}",
                    description=action,
                )
                steps.append(step)
        return steps

    @staticmethod
    def _build_plan_prompt(goal: str, memory: Memory) -> str:
        """Build a prompt for the LLM to generate plan steps."""
        history_summary = memory.summary()
        return (
            f"Given the following goal, break it down into clear, executable steps.\n\n"
            f"Goal: {goal}\n\n"
            f"Current context:\n{history_summary}\n\n"
            "Return a JSON array of steps, each with 'id', 'description', and 'required_tools'."
        )

    @staticmethod
    def _parse_plan_response(response: str, memory: Memory) -> list[PlanStep]:
        """Parse LLM response into PlanStep objects."""
        import json
        try:
            data = json.loads(response)
            steps = []
            for i, item in enumerate(data, 1):
                step = memory.add_plan_step(
                    step_id=f"step_{i}",
                    description=item.get("description", ""),
                )
                steps.append(step)
            return steps
        except (json.JSONDecodeError, TypeError):
            return [memory.add_plan_step("step_1", response.strip())]

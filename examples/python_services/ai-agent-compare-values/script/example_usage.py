"""
Example: Using the Plan/Action Agent Framework.

This example demonstrates:
1. Creating an agent with custom tools
2. Running a goal through the Plan/Action loop
3. Inspecting the execution results
"""

import sys
import os
import json

# Add the script directory to path
script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)
from memory import Memory
from tools import ToolRegistry, BaseTool
from agent import Agent
from default_tools import ReadFileTool, WriteFileTool, CalculatorTool


# --- Custom Tool Example ---

class GreetingTool(BaseTool):
    """A simple greeting tool for demonstration."""

    @property
    def name(self) -> str:
        return "greet"

    @property
    def description(self) -> str:
        return "Generate a greeting message for a person."

    @property
    def parameters(self) -> dict:
        return {
            "name": {"type": "string", "description": "Person's name"},
            "language": {"type": "string", "description": "Language (default: en)"},
        }

    def execute(self, name: str, language: str = "en") -> str:
        greetings = {
            "en": f"Hello, {name}! Nice to meet you.",
            "zh": f"你好，{name}！很高兴认识你。",
            "ja": f"{name}さん、こんにちは！",
            "fr": f"Bonjour, {name}! Enchante.",
        }
        return greetings.get(language, f"Hello, {name}!")


# --- Example 1: LLM-driven planning (recommended) ---

def example_llm_planning():
    """Example with LLM-driven planning - the recommended approach."""
    print("=" * 60)
    print("Example 1: LLM-Driven Planning (Recommended)")
    print("=" * 60)

    # Mock LLM client (replace with your actual LLM client)
    class MockLLMClient:
        def generate(self, prompt: str) -> str:
            return json.dumps([
                {"id": "step_1", "description": "greet: name=Alice, language=en"},
                {"id": "step_2", "description": "greet: name=Bob, language=zh"},
            ])

    agent = Agent(
        llm_client=MockLLMClient(),
        tools=[GreetingTool()],
        max_iterations=5,
        verbose=True,
    )

    result = agent.run("Greet Alice in English and Bob in Chinese")
    print(f"\nResult: {result['output']}")
    print(f"Success: {result['success']}")
    print(f"Iterations: {result['iterations']}")


# --- Example 2: File operations with LLM ---

def example_file_ops():
    """Example with file read/write operations."""
    print("\n" + "=" * 60)
    print("Example 2: File Operations with LLM")
    print("=" * 60)

    class MockLLMClient:
        def generate(self, prompt: str) -> str:
            return json.dumps([
                {"id": "step_1", "description": "read_file: path=/tmp/test_agent_input.txt"},
                {"id": "step_2", "description": "write_file: path=/tmp/test_agent_output.txt, content=HELLO WORLD"},
            ])

    # Create a test file
    test_file = "/tmp/test_agent_input.txt"
    with open(test_file, "w") as f:
        f.write("Hello World")

    agent = Agent(
        llm_client=MockLLMClient(),
        tools=[ReadFileTool(), WriteFileTool()],
        max_iterations=5,
        verbose=True,
    )

    result = agent.run(f"Read {test_file} and write uppercase content to /tmp/test_agent_output.txt")
    print(f"\nResult: {result['output']}")
    print(f"Success: {result['success']}")

    # Clean up
    os.remove(test_file)
    if os.path.exists("/tmp/test_agent_output.txt"):
        os.remove("/tmp/test_agent_output.txt")


# --- Example 3: Calculator with LLM ---

def example_calculator():
    """Example with calculator tool."""
    print("\n" + "=" * 60)
    print("Example 3: Calculator with LLM")
    print("=" * 60)

    class MockLLMClient:
        def generate(self, prompt: str) -> str:
            return json.dumps([
                {"id": "step_1", "description": "calculator: expression=42 * 13"},
            ])

    agent = Agent(
        llm_client=MockLLMClient(),
        tools=[CalculatorTool()],
        max_iterations=5,
        verbose=True,
    )

    result = agent.run("Calculate 42 * 13")
    print(f"\nResult: {result['output']}")
    print(f"Success: {result['success']}")


# --- Example 4: Custom tool integration ---

def example_custom_tool():
    """Example showing how to create and use custom tools."""
    print("\n" + "=" * 60)
    print("Example 4: Custom Tool Integration")
    print("=" * 60)

    class MockLLMClient:
        def generate(self, prompt: str) -> str:
            return json.dumps([
                {"id": "step_1", "description": "greet: name=World, language=ja"},
            ])

    agent = Agent(
        llm_client=MockLLMClient(),
        tools=[GreetingTool()],
        max_iterations=5,
        verbose=True,
    )

    result = agent.run("Greet World in Japanese")
    print(f"\nResult: {result['output']}")
    print(f"Success: {result['success']}")


# --- Example 5: Inspecting agent state ---

def example_inspect_state():
    """Example showing how to inspect agent state."""
    print("\n" + "=" * 60)
    print("Example 5: Inspecting Agent State")
    print("=" * 60)

    agent = Agent(
        tools=[GreetingTool(), CalculatorTool()],
        verbose=False,
    )

    print(f"Agent: {agent}")
    print(f"Available tools: {agent.list_tools()}")
    print(f"Memory summary: {agent.memory.summary()}")


if __name__ == "__main__":
    example_llm_planning()
    example_file_ops()
    example_calculator()
    example_custom_tool()
    example_inspect_state()

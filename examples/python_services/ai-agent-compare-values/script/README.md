# Plan/Action Agent Framework

A standard Python agent framework implementing the Plan/Action loop pattern.

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    Agent                         │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐ │
│  │  Planner │→ │ Executor │→ │    Memory      │ │
│  └──────────┘  └──────────┘  └───────────────┘ │
│         ▲                                    │
│         └──── ToolRegistry ──── BaseTool ────┘ │
└─────────────────────────────────────────────────┘
```

## Components

| Component    | File              | Description                              |
|-------------|-------------------|------------------------------------------|
| `Agent`     | `agent.py`        | Main orchestrator, runs Plan/Action loop  |
| `Planner`   | `planner.py`      | Breaks goals into executable steps        |
| `Executor`  | `executor.py`     | Executes plan steps by invoking tools     |
| `Memory`    | `memory.py`       | Stores conversation, plan state, context  |
| `ToolRegistry` | `tools.py`    | Manages available tools                   |
| `BaseTool`  | `tools.py`        | Abstract base class for all tools         |
| Default Tools | `default_tools.py` | Built-in tools (read, write, search, etc.) |

## Quick Start

```python
from script import Agent, BaseTool

# 1. Define a custom tool
class MyTool(BaseTool):
    @property
    def name(self): return "my_tool"
    @property
    def description(self): return "Do something useful"
    @property
    def parameters(self):
        return {"arg1": {"type": "string"}}
    def execute(self, arg1):
        return f"Result: {arg1}"

# 2. Create agent with LLM client
agent = Agent(
    llm_client=MyLLMClient(),  # Your LLM integration
    tools=[MyTool()],
    max_iterations=10,
    verbose=True,
)

# 3. Run a goal
result = agent.run("Your goal here")
print(result["output"])
```

## LLM Integration

The Planner uses an LLM to generate structured plan steps. Your LLM client must implement:

```python
class MyLLMClient:
    def generate(self, prompt: str) -> str:
        # Return JSON array of steps
        return json.dumps([
            {"id": "step_1", "description": "tool_name: arg=value"},
        ])
```

## Creating Custom Tools

```python
from script import BaseTool

class SearchTool(BaseTool):
    @property
    def name(self) -> str:
        return "search"

    @property
    def description(self) -> str:
        return "Search for patterns in text."

    @property
    def parameters(self) -> dict:
        return {
            "query": {"type": "string", "description": "Search query"},
        }

    def execute(self, query: str) -> str:
        # Your implementation
        return f"Results for: {query}"

# Register with agent
agent.add_tool(SearchTool())
```

## Step Format

Steps follow the format: `tool_name: arg1=value1, arg2=value2`

Examples:
- `greet: name=Alice, language=en`
- `read_file: path=/tmp/data.txt`
- `calculator: expression=42 * 13`

## API Reference

### Agent

| Method | Description |
|--------|-------------|
| `run(goal)` | Execute Plan/Action loop, returns dict |
| `run_step_by_step(goal)` | Execute one iteration (for debugging) |
| `add_tool(tool)` | Register a custom tool |
| `list_tools()` | List available tool names |

### Return Value

```python
{
    "success": bool,
    "iterations": int,
    "steps": [PlanStep, ...],
    "output": str,
}
```

### Memory

| Method | Description |
|--------|-------------|
| `add_message(role, content)` | Add a conversation message |
| `get_messages(role)` | Get messages by role |
| `get_pending_steps()` | Get pending plan steps |
| `get_completed_steps()` | Get completed plan steps |
| `summary()` | Get human-readable state summary |

## Files

```
script/
├── __init__.py          # Package exports
├── agent.py             # Main Agent class
├── planner.py           # Planner (LLM + rule-based)
├── executor.py          # Executor (step execution)
├── memory.py            # Memory (state management)
├── tools.py             # BaseTool + ToolRegistry
├── default_tools.py     # Built-in tools
└── example_usage.py     # Usage examples
```

## Running Examples

```bash
cd script
python example_usage.py
```

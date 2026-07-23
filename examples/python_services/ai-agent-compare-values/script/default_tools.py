"""
Default tools provided by the agent framework.
"""

import os
import re
import json
import math
from tools import BaseTool


class ReadFileTool(BaseTool):
    """Read the contents of a file."""

    @property
    def name(self) -> str:
        return "read_file"

    @property
    def description(self) -> str:
        return "Read the contents of a file given its path."

    @property
    def parameters(self) -> dict:
        return {
            "path": {"type": "string", "description": "The file path to read"},
            "encoding": {"type": "string", "description": "File encoding (default: utf-8)"},
        }

    def execute(self, path: str, encoding: str = "utf-8") -> str:
        try:
            with open(path, "r", encoding=encoding) as f:
                return f.read()
        except FileNotFoundError:
            raise FileNotFoundError(f"File not found: {path}")
        except Exception as e:
            raise RuntimeError(f"Error reading file: {e}")


class WriteFileTool(BaseTool):
    """Write content to a file."""

    @property
    def name(self) -> str:
        return "write_file"

    @property
    def description(self) -> str:
        return "Write content to a file, creating it if it doesn't exist."

    @property
    def parameters(self) -> dict:
        return {
            "path": {"type": "string", "description": "The file path to write"},
            "content": {"type": "string", "description": "Content to write"},
            "encoding": {"type": "string", "description": "File encoding (default: utf-8)"},
        }

    def execute(self, path: str, content: str, encoding: str = "utf-8") -> str:
        try:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            with open(path, "w", encoding=encoding) as f:
                f.write(content)
            return f"Successfully wrote {len(content)} characters to {path}"
        except Exception as e:
            raise RuntimeError(f"Error writing file: {e}")


class SearchTool(BaseTool):
    """Search text within files or strings."""

    @property
    def name(self) -> str:
        return "search"

    @property
    def description(self) -> str:
        return "Search for patterns in file contents or strings."

    @property
    def parameters(self) -> dict:
        return {
            "pattern": {"type": "string", "description": "Regex pattern to search for"},
            "content": {"type": "string", "description": "Text content to search in"},
            "file_path": {"type": "string", "description": "Optional file path to search in"},
            "case_sensitive": {"type": "boolean", "description": "Case sensitivity (default: False)"},
        }

    def execute(
        self,
        pattern: str,
        content: str | None = None,
        file_path: str | None = None,
        case_sensitive: bool = False,
    ) -> str:
        text = ""
        if file_path:
            with open(file_path, "r") as f:
                text = f.read()
        elif content:
            text = content
        else:
            raise ValueError("Provide either 'content' or 'file_path'")

        flags = 0 if case_sensitive else re.IGNORECASE
        matches = re.findall(pattern, text, flags)
        return json.dumps(matches, ensure_ascii=False)


class CalculatorTool(BaseTool):
    """Perform mathematical calculations."""

    @property
    def name(self) -> str:
        return "calculator"

    @property
    def description(self) -> str:
        return "Evaluate mathematical expressions safely."

    @property
    def parameters(self) -> dict:
        return {
            "expression": {"type": "string", "description": "Mathematical expression to evaluate"},
        }

    def execute(self, expression: str) -> str:
        # Safe evaluation: only allow math operations
        allowed = set("0123456789+-*/().% ")
        if not all(c in allowed for c in expression):
            raise ValueError("Only numeric expressions with +, -, *, /, (, ), ., % are allowed")

        try:
            result = eval(expression)  # noqa: S307 - safe because we validated characters
            return str(result)
        except Exception as e:
            raise ValueError(f"Invalid expression: {e}")


class JsonFormatTool(BaseTool):
    """Format or parse JSON data."""

    @property
    def name(self) -> str:
        return "json_format"

    @property
    def description(self) -> str:
        return "Parse and format JSON data with pretty printing."

    @property
    def parameters(self) -> dict:
        return {
            "data": {"type": "string", "description": "JSON string or dict to format"},
            "indent": {"type": "integer", "description": "Indentation spaces (default: 2)"},
        }

    def execute(self, data: str, indent: int = 2) -> str:
        try:
            parsed = json.loads(data) if isinstance(data, str) else data
            return json.dumps(parsed, ensure_ascii=False, indent=indent)
        except json.JSONDecodeError as e:
            raise ValueError(f"Invalid JSON: {e}")


class ListFilesTool(BaseTool):
    """List files in a directory."""

    @property
    def name(self) -> str:
        return "list_files"

    @property
    def description(self) -> str:
        return "List files and directories in a given path."

    @property
    def parameters(self) -> dict:
        return {
            "path": {"type": "string", "description": "Directory path to list"},
            "recursive": {"type": "boolean", "description": "List recursively (default: False)"},
            "pattern": {"type": "string", "description": "Optional glob pattern filter"},
        }

    def execute(self, path: str, recursive: bool = False, pattern: str | None = None) -> str:
        import glob
        import os

        if recursive:
            if pattern:
                files = glob.glob(os.path.join(path, "**", pattern), recursive=True)
            else:
                files = glob.glob(os.path.join(path, "**"), recursive=True)
        else:
            if pattern:
                files = glob.glob(os.path.join(path, pattern))
            else:
                files = os.listdir(path)

        return json.dumps([f for f in files if os.path.exists(f)], ensure_ascii=False)

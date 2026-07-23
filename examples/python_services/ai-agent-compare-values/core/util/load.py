import os

def load_text(file_path: str) -> str:
    """从指定路径加载文本文件内容"""
    if not os.path.isfile(file_path):
        raise FileNotFoundError(f"文件不存在: {file_path}")
    with open(file_path, 'r', encoding='utf-8') as f:
        return f.read()
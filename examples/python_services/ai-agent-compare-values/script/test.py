from typing import TypedDict, Literal
from langgraph.graph import StateGraph, START, END

# 1. 定义状态（类似 Protobuf 的 message 定义）
class DocumentState(TypedDict):
    # 当前处于哪个处理阶段
    current_step: Literal["loader", "parser", "summarizer", "reporter"]
    # 原始文档内容
    raw_document: str
    # 解析后的结构化数据
    parsed_data: dict | None
    # 生成的摘要
    summary: str | None
    # 错误信息
    error: str | None

# 2.1 加载文档节点
def load_document_node(state: DocumentState):
    # 模拟加载文档
    print(f"📄 正在加载文档: {state['raw_document'][:30]}...")
    # 这里可以添加真实的加载逻辑
    return {"current_step": "parser"} # 更新当前步骤，并流转到下一步

# 2.2 解析文档节点
def parse_document_node(state: DocumentState):
    print("🔍 正在解析文档...")
    # 模拟解析，提取一些数据
    parsed = {"title": "示例文档", "word_count": len(state["raw_document"].split())}
    return {"parsed_data": parsed, "current_step": "summarizer"}

# 2.3 生成摘要节点
def summarize_document_node(state: DocumentState):
    print("📝 正在生成摘要...")
    # 模拟生成摘要
    summary = f"文档摘要: 共 {state['parsed_data']['word_count']} 个词"
    return {"summary": summary, "current_step": "reporter"}

# 2.4 生成报告节点
def generate_report_node(state: DocumentState):
    print("📊 正在生成最终报告...")
    report = f"""
    ---- 处理报告 ----
    标题: {state['parsed_data']['title']}
    摘要: {state['summary']}
    """
    print(report)
    return {"current_step": "reporter"}

# 3. 构建图
builder = StateGraph(DocumentState)

# 添加节点
builder.add_node("loader", load_document_node)
builder.add_node("parser", parse_document_node)
builder.add_node("summarizer", summarize_document_node)
builder.add_node("reporter", generate_report_node)

# 添加边，定义流转路径（固定流转）
builder.add_edge(START, "loader")
builder.add_edge("loader", "parser")
builder.add_edge("parser", "summarizer")
builder.add_edge("summarizer", "reporter")
builder.add_edge("reporter", END)

# 编译状态机（相当于生成代码）
graph = builder.compile()

# 4. 运行状态机
if __name__ == "__main__":
    initial_state = {
        "current_step": "loader",
        "raw_document": "这是一个关于LangGraph的示例文档，用于演示如何构建状态机。",
        "parsed_data": None,
        "summary": None,
        "error": None,
    }
    
    # 执行状态机
    final_state = graph.invoke(initial_state)
    print("\n✅ 状态机执行完毕，最终状态:")
    for key, value in final_state.items():
        print(f"  {key}: {value}")
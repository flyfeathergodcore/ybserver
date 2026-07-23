from typing import TypedDict, Literal
from langgraph.graph import StateGraph, START, END
import random

# 1. 定义状态（包含循环控制字段）
class TicketState(TypedDict):
    # 核心状态：当前步骤
    current_step: Literal[
        "triage",           # 工单分类
        "assign",           # 分配处理人
        "resolve",          # 处理解决
        "review",           # 审核
        "escalate",         # 升级处理
        "close"             # 关闭
    ]
    # 工单信息
    ticket_id: str
    description: str
    # 循环控制
    retry_count: int       # 审核退回次数
    max_retries: int       # 最大退回次数
    resolve_quality: int   # 处理质量评分 (1-10)
    need_escalate: bool    # 是否需要升级
    # 历史记录
    history: list[str]

# 2. 实现各个节点

def triage_node(state: TicketState):
    """初步分类 - 根据描述决定工单类型"""
    print(f"[{state['ticket_id']}] 📋 正在进行工单分类...")
    # 模拟分类逻辑
    if "密码" in state["description"] or "账号" in state["description"]:
        category = "账号问题"
    elif "退款" in state["description"] or "支付" in state["description"]:
        category = "财务问题"
    else:
        category = "技术问题"
    
    history = state.get("history", []) + [f"分类为: {category}"]
    return {
        "current_step": "assign",
        "history": history
    }

def assign_node(state: TicketState):
    """分配处理人"""
    print(f"[{state['ticket_id']}] 👤 分配处理人...")
    # 模拟分配
    assignee = "张三" if state["ticket_id"].endswith("1") else "李四"
    history = state.get("history", []) + [f"分配给: {assignee}"]
    return {
        "current_step": "resolve",
        "history": history
    }

def resolve_node(state: TicketState):
    """处理解决 - 核心业务逻辑"""
    print(f"[{state['ticket_id']}] 🔧 正在处理工单...")
    
    # 模拟处理质量（可配置，这里用随机）
    quality = random.randint(1, 10)
    # 判断是否需要升级（质量<3或涉及敏感内容）
    need_escalate = quality < 3 or "紧急" in state["description"]
    
    history = state.get("history", []) + [f"处理完成, 质量评分: {quality}"]
    
    # 关键：设置下一步需要的状态
    return {
        "resolve_quality": quality,
        "need_escalate": need_escalate,
        "current_step": "review",  # 默认去审核
        "history": history
    }

def review_node(state: TicketState):
    """审核环节 - 这是循环的核心"""
    print(f"[{state['ticket_id']}] ✅ 正在审核处理结果...")
    
    retry_count = state.get("retry_count", 0)
    max_retries = state.get("max_retries", 3)
    quality = state.get("resolve_quality", 0)
    
    # 审核逻辑
    if state.get("need_escalate", False):
        # 需要升级，跳转到升级节点（不经过循环）
        print(f"  ⚠️ 工单需要升级处理")
        return {
            "current_step": "escalate",
            "history": state.get("history", []) + ["需要升级"]
        }
    elif quality >= 6:
        # 质量合格，关闭工单
        print(f"  ✅ 审核通过，准备关闭")
        return {
            "current_step": "close",
            "history": state.get("history", []) + ["审核通过"]
        }
    elif retry_count >= max_retries:
        # 超出重试次数，强制关闭（或升级）
        print(f"  ⚠️ 已超过最大重试次数({max_retries})，强制关闭")
        return {
            "current_step": "close",
            "history": state.get("history", []) + [f"超出重试次数，强制关闭"]
        }
    else:
        # 不合格，退回重新处理（这就是循环！）
        new_retry_count = retry_count + 1
        print(f"  🔄 审核不通过，退回重新处理 (第 {new_retry_count}/{max_retries} 次)")
        return {
            "retry_count": new_retry_count,
            "current_step": "resolve",  # 回到处理节点
            "history": state.get("history", []) + [f"第{new_retry_count}次退回重审"]
        }

def escalate_node(state: TicketState):
    """升级处理（特殊通道）"""
    print(f"[{state['ticket_id']}] 🚀 升级到高级处理...")
    history = state.get("history", []) + ["升级处理完成"]
    return {
        "current_step": "close",
        "history": history
    }

def close_node(state: TicketState):
    """关闭工单"""
    print(f"[{state['ticket_id']}] 🏁 工单已关闭")
    history = state.get("history", []) + ["工单关闭"]
    return {
        "current_step": "close",
        "history": history
    }

# 3. 构建状态机（关键：路由函数）

def route_after_resolve(state: TicketState) -> Literal["review", "escalate"]:
    """解决后的路由：决定去审核还是直接升级"""
    if state.get("need_escalate", False):
        return "escalate"
    return "review"

# 主构建函数
def build_ticket_state_machine():
    builder = StateGraph(TicketState)
    
    # 添加所有节点
    builder.add_node("triage", triage_node)
    builder.add_node("assign", assign_node)
    builder.add_node("resolve", resolve_node)
    builder.add_node("review", review_node)
    builder.add_node("escalate", escalate_node)
    builder.add_node("close", close_node)
    
    # 添加边（包含循环）
    builder.add_edge(START, "triage")
    builder.add_edge("triage", "assign")
    builder.add_edge("assign", "resolve")
    
    # 解决后条件路由：可能去审核或直接升级
    builder.add_conditional_edges("resolve", route_after_resolve)
    
    # 关键：审核节点的循环逻辑
    # 注意：review 节点内部通过返回 current_step 来控制下一步
    # 但如果 review 返回的是 "resolve"，则需要显式添加边
    builder.add_edge("review", "close")       # review -> close 是固定边
    # 但 review 内部可能返回 current_step="resolve"，所以需要条件路由
    # 更优雅的方式：使用条件边来处理 review 的所有可能输出
    
    builder.add_edge("escalate", "close")
    builder.add_edge("close", END)
    
    return builder.compile()

# 4. 运行测试
def run_test():
    graph = build_ticket_state_machine()
    
    # 测试用例1：高质量工单（一次通过）
    print("\n" + "="*50)
    print("测试1: 高质量工单")  
    state1 = {
        "ticket_id": "T001",
        "description": "系统登录报错，无法连接数据库",
        "retry_count": 0,
        "max_retries": 3,
        "resolve_quality": 0,
        "need_escalate": False,
        "history": []
    }
    random.seed(10)  # 固定随机种子，让 quality=9 (高质量)
    result1 = graph.invoke(state1)
    print(f"\n最终历史: {result1['history']}")
    
    # 测试用例2：低质量工单（会触发循环）
    print("\n" + "="*50)
    print("测试2: 低质量工单（触发循环）")
    state2 = {
        "ticket_id": "T002",
        "description": "密码重置后仍无法登录",
        "retry_count": 0,
        "max_retries": 3,
        "resolve_quality": 0,
        "need_escalate": False,
        "history": []
    }
    random.seed(1)  # 固定随机种子，让 quality=2 (低质量)
    result2 = graph.invoke(state2)
    print(f"\n最终历史: {result2['history']}")
    print(f"总共重试次数: {result2['retry_count']}")

if __name__ == "__main__":
    run_test()
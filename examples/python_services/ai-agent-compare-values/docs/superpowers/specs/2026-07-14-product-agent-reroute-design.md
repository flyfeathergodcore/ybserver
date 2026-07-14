# 产品 Agent 接管设计 — Server 编排 + Product Agent 自主对话

> **设计目标：** 导购 agent 信息收集完毕后，server 将会话控制权移交给 product agent。Product agent 从 MCP 读取 intent，自主搜索、推荐、答疑，不再由导购 agent 代理调用 product agent。

---

## 1. 背景与动机

### 当前问题

当前导购 agent 的 `_handle_final()` 直接调用 `product_agent.search_by_guide_intent()`，存在三个问题：

1. **紧耦合**：导购 agent 持有 product_agent 引用并直接调用其方法，职责不清
2. **跳过了 MCP 中间层**：MCP 已设计 `session_intent` 节点作为意图持久化通道，但实际未使用
3. **服务端被动**：服务端只能从 `guide._recommended_products` 读取结果，没有真正的控制流程

### 新的角色划分

| 阶段 | 负责 Agent | 职责 |
|------|-----------|------|
| 需求收集 | 导购 Agent | 对话收集品类/预算/约束，生成 `session_intent` |
| 产品搜索与推荐 | 产品 Agent | 从 MCP 读 intent，搜索、推荐、答疑、对比 |
| 会话编排 | Server | 追踪 stage，路由消息，不参与业务逻辑 |

---

## 2. 架构

### 会话状态

`mcp_setup.py` 中的 session dict 扩展为：

```python
sessions[sid] = {
    "guide": guide,             # ShoppingGuideAgent
    "product": product,         # ProductAgent
    "stage": "guide",           # "guide" | "product"
    "product_history": [],      # product 阶段对话记录 [{role, content}]
    "last_action": None,        # 最近一次 product agent 动作（用于日志/调试）
}
```

### 数据流

```
 用户消息
    │
    ▼
┌──────────────────────────────────────────────────┐
│  Server (grpc_server.py / web_server.py)         │
│                                                   │
│  1. 查 session["stage"]                           │
│     ├─ "guide"  → guide.analyze_l1(messages)     │
│     │              └─ 返回 {"final": True}        │
│     │                 → session["stage"] = "product"
│     │                 → product.product_main_loop(first_call=True)
│     │
│     └─ "product" → product.product_main_loop(    │
│                       user_message=msg,           │
│                       history=product_history     │
│                    )                               │
│                                                   │
│  2. 每次 product_main_loop 返回后自动保存状态     │
│                                                   │
│  3. 格式化回复 → 推给前端                         │
└──────────────────────────────────────────────────┘
```

---

## 3. 组件设计

### 3.1 导购 Agent — `_handle_final()` 改造

**当前行为：** 调 `product_agent.search_by_guide_intent()` + 设置 `_recommended_products`

**新行为：** 只存 intent，不碰搜索

```python
def _handle_final(self, final_content: dict) -> dict:
    """
    LLM 认为信息已收集完毕 → 将 intent 持久化到 MCP，返回 final 信号。
    
    不调 product_agent，不搜索，不设 _recommended_products。
    """
    if not final_content:
        return {"final": True, "product_interacted": False}
    
    # 1. 持久化 intent 到 MCP
    self.exec_tool("store_memory",
        node_type="session_intent",
        content=json.dumps(final_content, ensure_ascii=False),
        importance=0.9,
        tags=f"session:{self._session_id}",
    )
    
    # 2. 保存导购阶段的完整会话（更新用户画像等）
    self.end_session(context="导购阶段完成，转入产品搜索")
    
    # 3. 清理内部状态（保留 _recommended_products 供调试，不再被 server 读取）
    return {"final": True, "product_interacted": False}
```

### 3.2 Product Agent — `product_main_loop()` 新增

#### 方法签名

```python
def product_main_loop(
    self,
    session_id: str,
    first_call: bool = False,
    user_message: str = "",
    history: list[dict] | None = None,
) -> dict:
    """
    产品 Agent 主循环 — 自主搜索/推荐/答疑/对比
    
    Args:
        session_id: 会话 ID，用于从 MCP 恢复状态
        first_call: True=首次调用，从 MCP 读 intent 并搜索
        user_message: 后续调用的用户消息
        history: 产品阶段的对话历史
    
    Returns:
        {"reply": str, "candidates": list[dict]}
    """
```

#### 首次调用流程（first_call=True）

```
load_intent_from_mcp(session_id)
    → query_memory_by_type("session_intent")
    → 解析 intent 中的 category / budget / constraints

search_by_guide_intent(intent)
    → ware_search / jingfen_query / rank_query 三级降级
    → bigfield_query 补全详情
    → 保存到 self._current_analysis + self._current_candidates

_product_llm_first_response(analysis, intent)
    → LLM 生成回复：
        1. "根据你的需求找到 N 款产品"
        2. 推荐 top 3 + 理由（每款 1-2 句）
        3. "你可以问我某款产品的详情、对比、或者告诉我需求调整"
    → 返回 {"reply": LLM回复, "candidates": candidates[:10]}
```

#### 多轮对话流程（first_call=False）

```
append user_message → self._product_history

_product_llm_with_tools(history)
    → 循环：
        LLM 输出 action → 分支处理：
        
        tool:search   → ware_search/jingfen_query → 更新 candidates → 继续
        tool:detail   → answer_product_question() → 追加 context → 继续
        tool:compare  → compare_products() → 追加对比结果 → 继续  
        tool:recommend → generate_recommendations() → 追加评分 → 继续
        纯文本        → 返回 {"reply": 文本, "candidates": candidates}
    
    → 每次返回前自动保存 session 状态
```

#### 四种 Tool Action 定义

Product agent 的 LLM 输出使用相同的 `[agent_analyse]:...[json]:...` 格式，复用 `json_parse.py`：

**search — 重新搜索**
```json
{"tool": true, "tool_name": "search", "kwargs": {
    "keyword": "笔记本电脑",
    "price_min": 5000,
    "price_max": 8000,
    "brand": "联想",
    "constraints": ["独立显卡", "16GB内存"]
}}
```

**detail — 查产品详情**
```json
{"tool": true, "tool_name": "detail", "kwargs": {
    "product_id": "100215674663"
}}
```

**compare — 对比产品**
```json
{"tool": true, "tool_name": "compare", "kwargs": {
    "product_ids": ["100215674663", "100215674664"]
}}
```

**recommend — 生成推荐评分**
```json
{"tool": true, "tool_name": "recommend", "kwargs": {}}
```

**纯文本回复** — 无 JSON，直接返回自然语言

### 3.3 Product Agent System Prompt

新建 `prompts/product_chat.txt`：

```txt
你是京东产品推荐助手。你的任务是基于用户的购买需求和产品数据，帮助用户找到最合适的产品。

## 当前搜索结果
{candidates}

## 用户购买意图
{intent}

## 可调用的工具（通过 JSON 输出调用）
1. search(keyword, price_min, price_max, brand, constraints)
   用户说"太贵了"、"换个品牌"、"找更便宜的"时调用。
   新的搜索结果会替换当前推荐列表。

2. detail(product_id)
   用户问某款产品的详情时调用。
   系统会返回该产品的完整参数、价格走势、市场定位。

3. compare(product_ids)
   用户要对比几款产品时调用。
   系统会返回参数+价格的横向对比表。

4. recommend()
   无需用户提问，主动生成综合推荐评分。

## 行为规则
- 首次进入时：列出 top 3 推荐 + 推荐理由。告知用户共有 N 款候选产品。
  每款推荐理由控制在 1-2 句，引用具体参数（如"i7-13700H处理器"、"32GB内存"）。
  最后询问用户是否需要深入了解某款、对比、或调整需求。

- 用户问详情：调用 detail 获取参数，用口语化语言回答。
  不要罗列全部参数，只挑用户可能关心的。

- 用户说"太贵"/"便宜点的"/"换个品牌"：调用 search 重新搜索。
  如果搜索范围太宽，先展示结果再问需不需要细化。

- 用户对比：调用 compare 获取对比表，给出你的判断建议。

- 回答要口语化中文，简短直接。不要输出 JSON 或系统提示给用户看。
- 如果你认为信息充足，可以主动推荐 "最推荐的是XX，因为……"
```

### 3.4 Server 路由改造

#### grpc_server.py ChatStream

```python
async def ChatStream(self, request, context):
    _ensure_initialized()
    msg = request.message
    sid = request.session_id or f"sess_{int(time.time())}"
    sess = _get_or_create_session(sid)
    guide = sess["guide"]
    product = sess["product"]
    
    yield shopping_pb2.ShoppingEvent(meta=shopping_pb2.MetaEvent(
        session_id=sid, stage=sess["stage"], ready=False))
    
    try:
        if sess["stage"] == "guide":
            result = guide.analyze_l1([{"role": "user", "content": msg}])
            
            if result.get("final") is True:
                # 切换到产品阶段
                sess["stage"] = "product"
                result = product.product_main_loop(
                    session_id=sid, first_call=True)
                
        elif sess["stage"] == "product":
            result = product.product_main_loop(
                session_id=sid, user_message=msg, history=product_history)
                
    except Exception as e:
        yield shopping_pb2.ShoppingEvent(error=...)
        return
    
    # 统一格式化回复
    reply = result.get("reply", "")
    candidates = result.get("candidates", [])
    # → 转成 shopping_pb2.Candidate 列表 → yield
```

#### web_server.py

相同的 stage routing 逻辑，同步修改。

### 3.5 会话持久化（支持恢复）

#### 自动保存（每次 product_main_loop 返回后）

```python
def _save_product_session(self, session_id: str):
    """保存当前产品阶段状态到 MCP，支持用户 1 天后回来继续"""
    state = {
        "stage": "product",
        "intent": self._current_intent,
        "candidates": self._current_candidates[:10],
        "history": self._product_history[-30:],  # 最近 30 轮
    }
    self._call_mcp("store_memory",
        node_type="product_session",
        content=json.dumps(state, ensure_ascii=False),
        importance=0.8,
        tags=f"session:{session_id}",
    )
```

#### 恢复流程（server 层）

```python
# grpc_server 创建 session 后
def _maybe_resume_product(sess):
    """检查是否有已保存的 product 状态"""
    state = product._load_product_session(session_id)
    if state and state.get("stage") == "product":
        sess["stage"] = "product"
        product.resume_from_state(state)
```

---

## 4. 文件改动清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `core/guide_agent.py` | 修改 | `_handle_final()` 改为只存 intent 到 MCP，不调 product agent |
| `core/product_agent.py` | 新增方法 | `product_main_loop()`, `_product_llm_with_tools()`, `_product_llm_first_response()`, `_load_intent_from_mcp()`, `_save_product_session()`, `_parse_product_action()` |
| `prompts/product_chat.txt` | 新建 | 产品 agent system prompt，约 40-50 行 |
| `grpc_server.py` | 修改 | ChatStream 增加 stage routing |
| `web_server.py` | 修改 | `_handle_chat_stream` 增加 stage routing |
| `mcp_servers/mcp_setup.py` | 修改 | session dict 增加 stage/product_history/last_action 字段 |

---

## 5. 不变的部分

- `run.py` — 不变，继续用 `create_mcp_services` + `create_session_factory`
- `core/llm_client.py` — 不变
- `core/share_utils.py` — 不变
- `tools/json_parse.py` — 不变（复用 `_construct_fallback_json`）
- 所有 MCP skills — 不变
- `core/config.py` — 不变
- `core/__init__.py` — 不变

---

## 6. 边界情况

| 场景 | 处理方式 |
|------|---------|
| Product agent 第一次 search 返回空 | LLM 回复"未找到符合条件的产品"，建议调整预算/品类 |
| 用户换到了完全不相关的产品话题 | Product agent 继续处理（它覆盖产品全流程），但如果变成了非产品问题（如"帮我写个文档"），保持引导回产品话题 |
| 用户长时间不回复 | 已有自动保存机制，下次回来恢复 |
| 会话被用户重置（reset） | 清理 MCP 中对应的 session_intent + product_session |
| 导购 agent 未输出 final 前用户关闭页面 | 下次回来仍然在 guide 阶段，导购 agent 继续对话 |
| 导购 final_content 为空或格式异常 | product agent 检测到后回复"导购信息不完整，请重新描述需求" |

---

## 7. 后续扩展

- **产品 agent 可直接调 MCP 的 product 组工具**：目前走 `_call_mcp` 封装，未来无需改动
- **定时刷新逻辑保持不变**：`refresh_product_db` 由 product agent 构造时启动，与对话流程无关
- **多轮搜索推荐 LLM 的 temperature 可调**：通过 `product_chat_temperature` 配置项控制

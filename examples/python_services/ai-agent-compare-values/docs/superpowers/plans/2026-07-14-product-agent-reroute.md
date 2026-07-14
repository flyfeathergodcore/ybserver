# 产品 Agent 接管实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 导购 agent 收集需求完毕后，server 将会话控制权移交给 product agent，product agent 从 MCP 读取 intent 并自主搜索/推荐/答疑。

**Architecture:** Server 追踪 `session["stage"]`（guide → product），路由消息到对应 agent。Product agent 新增 `product_main_loop()`，拥有完整 LLM 对话+工具调用循环，复用现有 MCP 搜索/详情/对比工具。每次回复后自动保存会话状态到 MCP，支持用户 1 天后回来继续。

**Tech Stack:** Python 3.10+, MCP DirectClient, LLMClient (OpenAI-compatible)

## Global Constraints

- 使用已有的 `[agent_analyse]:...[json]:...` 输出格式，复用 `json_parse.py`
- Product agent 不输出 `final` 类型，永不主动结束对话
- 所有文件在 `examples/python_services/ai-agent-compare-values/` 目录下
- 保持口语化中文回复，不输出 JSON/系统提示给用户

---

### Task 1: 导购 Agent _handle_final 改造

**Files:**
- Modify: `core/guide_agent.py`（_handle_final 方法 + __init__ 移除 product_agent 参数）

**Interfaces:**
- Consumes: `self.exec_tool("store_memory", ...)`, `self.end_session()`
- Produces: `{"final": True, "product_interacted": False}`

**改动内容：** `_handle_final` 不再调 `product_agent.search_by_guide_intent()`，改为将 `final_content` 存到 MCP，然后返回 final 信号。同时从 `__init__` 中移除 `product_agent` 参数（不再需要）。

```python
# ── 替换 _handle_final 全部内容 ──

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
    
    return {"final": True, "product_interacted": False}
```

删除 `analyze_l1` 中的 `_handle_final` 调用，改为直接返回 final 信号给 server：

```python
# ── analyze_l1 中 ──
elif result.get("final") is True:
    return self._handle_final(result.get("final_content", {}))
    # _handle_final 现在返回 {"final": True, "product_interacted": False}
    # server 收到后切 stage，不依赖 reply 字段
```

- [ ] **Step 1: 从 __init__ 中移除 product_agent 参数**

```python
# __init__ 签名改为：
def __init__(
    self,
    llm: LLMClient,
    mcp: Any = None,
    config: Optional[dict] = None,
    # 删除: product_agent: Any = None,
    ...
):
```

同时删除 `self._product_agent = product_agent` 这一行。

- [ ] **Step 2: 替换 _handle_final 方法**

```python
def _handle_final(self, final_content: dict) -> dict:
    if not final_content:
        return {"final": True, "product_interacted": False}
    self.exec_tool("store_memory",
        node_type="session_intent",
        content=json.dumps(final_content, ensure_ascii=False),
        importance=0.9,
        tags=f"session:{self._session_id}",
    )
    self.end_session(context="导购阶段完成，转入产品搜索")
    return {"final": True, "product_interacted": False}
```

- [ ] **Step 3: 验证语法**

```bash
cd examples/python_services/ai-agent-compare-values
python -c "import ast; ast.parse(open('core/guide_agent.py').read()); print('OK')"
```

- [ ] **Step 4: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/core/guide_agent.py
git commit -m "refactor(guide_agent): _handle_final 改为只存 intent 到 MCP

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: 创建 Product Agent System Prompt

**Files:**
- Create: `prompts/product_chat.txt`

**Interfaces:**
- Consumes: 无
- Produces: prompt 文件，Task 3 的 `product_main_loop` 读取使用

```txt
你是京东产品推荐助手。你的任务是基于用户的购买需求和产品数据，帮助用户找到最合适的产品。

## 当前搜索结果
{candidates}

## 用户购买意图
{intent}

## 可调用的工具（通过 JSON 输出调用）
{mcp_tools}

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

## 可输出 Action 类型

### 1. search — 重新搜索
当用户要求调整价格/品牌/配置时，输出：
[json]:{"tool":true,"tool_name":"search","kwargs":{"keyword":"品类","price_min":0,"price_max":0,"brand":"品牌","constraints":["要求1","要求2"]}}

### 2. detail — 查产品详情
当用户问某款产品的参数/评价/详情时，输出：
[json]:{"tool":true,"tool_name":"detail","kwargs":{"product_id":"产品ID"}}

### 3. compare — 对比产品
当用户要对比多款产品时，输出：
[json]:{"tool":true,"tool_name":"compare","kwargs":{"product_ids":["id1","id2"]}}

### 4. recommend — 生成推荐评分
当需要综合评分时，输出：
[json]:{"tool":true,"tool_name":"recommend","kwargs":{}}

### 5. 纯文本回复
不需要调用工具时，直接输出自然语言对话，不包含 [json]: 标记。
```

- [ ] **Step 1: 创建 prompts/product_chat.txt**

```bash
touch prompts/product_chat.txt
```
然后写入上述内容。

- [ ] **Step 2: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/prompts/product_chat.txt
git commit -m "feat(product_agent): 创建 product_chat system prompt

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Product Agent 新增 product_main_loop

**Files:**
- Modify: `core/product_agent.py`（新增方法，不删除已有方法）

**Interfaces:**
- Consumes: `prompts/product_chat.txt`（Task 2）, `json_parse._construct_fallback_json` 回退
- Produces: `product_main_loop(session_id, first_call, user_message, history) -> dict`

新增以下方法到 `ProductAgent` 类：

```python
# ── 新增实例变量（在 __init__ 末尾添加）──
self._product_history: list[dict] = []
self._current_intent: dict = {}
self._current_candidates: list[dict] = []
self._current_analysis: Any = None  # ProductAnalysis，用于 recommend 工具
self._product_chat_prompt = self._load_file(os.path.join(
    self._config_dir, "prompts", "product_chat.txt"))
```

**product_main_loop 方法：**

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
        history: 产品阶段的对话历史（优先于 self._product_history）
    
    Returns:
        {"reply": str, "candidates": list[dict]}
    """
    if history is not None:
        self._product_history = history
    
    if first_call:
        # 首次调用：从 MCP 读 intent → 搜索
        self._current_intent = self._load_intent_from_mcp(session_id)
        if not self._current_intent:
            return {"reply": "导购信息不完整，请重新描述需求", "candidates": []}
        
        self._current_analysis = self.search_by_guide_intent(self._current_intent)
        self._current_candidates = (self._current_analysis.candidates or [])[:10]
        
        if not self._current_candidates:
            return {"reply": "未找到符合需求的产品，请调整需求", "candidates": []}
        
        # LLM 生成首次回复
        reply = self._product_llm_first_response()
        self._product_history.append({"role": "agent", "content": reply})
    else:
        # 后续调用：LLM 工具循环
        self._product_history.append({"role": "user", "content": user_message})
        reply = self._product_llm_with_tools()
        self._product_history.append({"role": "agent", "content": reply})
    
    # 自动保存状态
    self._save_product_session(session_id)
    
    return {"reply": reply, "candidates": self._current_candidates}
```

**_load_intent_from_mcp 方法：**

```python
def _load_intent_from_mcp(self, session_id: str) -> dict:
    """从 MCP 读取导购 agent 存储的 session_intent"""
    result = self._call_mcp("query_memory_by_type",
        mem_type="session_intent", limit=10)
    if not result:
        return {}
    
    nodes = result.get("profiles", result.get("nodes", []))
    if isinstance(result, list):
        nodes = result
    
    # 按 session_id 过滤
    for node in nodes:
        if isinstance(node, dict):
            content = node.get("content", "")
            tags = node.get("tags", "")
            if session_id in tags:
                try:
                    return json.loads(content) if isinstance(content, str) else content
                except json.JSONDecodeError:
                    return content
    # 如果 sessions_id 不匹配，取最新的
    for node in nodes:
        if isinstance(node, dict):
            content = node.get("content", "")
            try:
                return json.loads(content) if isinstance(content, str) else content
            except json.JSONDecodeError:
                return content
    return {}
```

**_product_llm_first_response 方法：**

```python
def _product_llm_first_response(self) -> str:
    """基于搜索结果生成首次回复"""
    candidates = self._current_candidates
    intent = self._current_intent
    
    # 获取 MCP 工具列表
    mcp_tools = ""
    if self._mcp:
        try:
            tools = self._mcp.skill_list()
            mcp_tools = json.dumps([{"name": t["name"], "params": t.get("params", {})} for t in tools], 
                                    ensure_ascii=False, indent=2)
        except Exception:
            mcp_tools = "（MCP 工具列表不可用）"
    
    prompt = self._product_chat_prompt.format(
        candidates=json.dumps(candidates[:3], ensure_ascii=False, indent=2)[:2000],
        intent=json.dumps(intent, ensure_ascii=False, indent=2),
        mcp_tools=mcp_tools,
    )
    
    return self.llm.chat(prompt, system_prompt="你是京东产品推荐助手。", temperature=0.3)
```

**_product_llm_with_tools 方法：**

```python
def _product_llm_with_tools(self, max_iterations: int = 3) -> str:
    """产品 Agent LLM 工具循环 — 处理 search/detail/compare/recommend"""
    iteration = 0
    while iteration < max_iterations:
        iteration += 1
        
        # 获取 MCP 工具列表
        mcp_tools = ""
        if self._mcp:
            try:
                tools = self._mcp.skill_list()
                mcp_tools = json.dumps([{"name": t["name"], "params": t.get("params", {})} for t in tools[:10]],
                                        ensure_ascii=False, indent=2)
            except Exception:
                mcp_tools = "（MCP 工具列表不可用）"
        
        # 构建 prompt
        history_text = "\n".join(
            f"[{d['role']}]: {d['content'][:500]}" for d in self._product_history[-10:]
        )
        candidates_text = json.dumps(self._current_candidates[:3], ensure_ascii=False, indent=2)[:2000]
        
        prompt = self._product_chat_prompt.format(
            candidates=candidates_text,
            intent=json.dumps(self._current_intent, ensure_ascii=False, indent=2),
            mcp_tools=mcp_tools,
        )
        prompt += f"\n\n## 当前对话\n{history_text}\n\n## 请根据对话和结果生成回复或调用工具"
        
        raw = self.llm.chat(prompt, system_prompt="你是京东产品推荐助手。", temperature=0.3)
        
        # 解析 action
        action = self._parse_product_action(raw)
        if action is None:
            return raw.strip()  # 纯文本回复
        
        action_type = action.get("type")
        kwargs = action.get("kwargs", {})
        
        if action_type == "search":
            keyword = kwargs.get("keyword", "")
            brand = kwargs.get("brand", "")
            price_min = kwargs.get("price_min", 0)
            price_max = kwargs.get("price_max", 99999)
            constraints = kwargs.get("constraints", [])
            
            # 更新 intent
            if keyword:
                self._current_intent["category"] = keyword
            self._current_intent["intent_delta"] = self._current_intent.get("intent_delta", {})
            if brand:
                self._current_intent["intent_delta"]["brand"] = brand
            if price_min or price_max:
                self._current_intent["intent_delta"]["budget"] = {
                    "min": price_min, "max": price_max,
                    "current": price_max, "confidence": "medium",
                }
            if brand:
                self._current_intent["intent_delta"]["constraints"] = constraints
            
            # 重新搜索
            self._current_analysis = self.search_by_guide_intent(self._current_intent)
            self._current_candidates = (self._current_analysis.candidates or [])[:10]
            
            tool_output = f"搜索到 {len(self._current_candidates)} 款产品"
            self._product_history.append({"role": "tool", "content": tool_output})
            
        elif action_type == "detail":
            product_id = kwargs.get("product_id", "")
            if product_id:
                detail = self.answer_product_question(
                    f"介绍一下产品 {product_id}",
                    candidates=self._current_candidates,
                )
                self._product_history.append({"role": "tool", "content": detail[:1000]})
                
        elif action_type == "compare":
            pids = kwargs.get("product_ids", [])
            if pids:
                result = self.compare_products(pids)
                self._product_history.append({
                    "role": "tool",
                    "content": json.dumps(result, ensure_ascii=False)[:1000],
                })
                
        elif action_type == "recommend":
            # 生成推荐评分
            intent_info = {"session_intent": self._current_intent, "user_profile": {}}
            recs = self.generate_recommendations(
                self._current_analysis, intent_info) if self._current_analysis else []
            self._product_history.append({
                "role": "tool",
                "content": json.dumps([{"score": r.score, "reasons": r.reasons} for r in recs], ensure_ascii=False)[:1000],
            })
    
    return "请问还有什么可以帮你的？"
```

**_parse_product_action 方法：**

```python
@staticmethod
def _parse_product_action(raw: str) -> dict | None:
    """
    解析 product agent LLM 的 action 输出。
    格式与 guide agent 相同: [json]:{"tool":true,"tool_name":"...","kwargs":{...}}
    
    返回:
        {"type": "search"|"detail"|"compare"|"recommend", "kwargs": {...}} | None
    """
    if not raw:
        return None
    
    # 复用 json_parse 的 JSON 提取
    from tools.json_parse import _extract_json_from_text
    result = _extract_json_from_text(raw)
    
    if result and result.get("tool") is True:
        tool_name = result.get("tool_name", "")
        kwargs = result.get("kwargs", {})
        return {"type": tool_name, "kwargs": kwargs}
    
    return None
```

**_save_product_session 方法：**

```python
def _save_product_session(self, session_id: str):
    """保存当前产品阶段状态到 MCP"""
    state = {
        "stage": "product",
        "intent": self._current_intent,
        "candidates": self._current_candidates[:10],
        "history": self._product_history[-30:],
    }
    self._call_mcp("store_memory",
        node_type="product_session",
        content=json.dumps(state, ensure_ascii=False),
        importance=0.8,
        tags=f"session:{session_id}",
    )
```

- [ ] **Step 1: 在 __init__ 末尾添加实例变量**

```python
self._product_history: list[dict] = []
self._current_intent: dict = {}
self._current_candidates: list[dict] = []
self._product_chat_prompt = self._load_file(os.path.join(
    self._config_dir, "prompts", "product_chat.txt"))
```

- [ ] **Step 2: 添加 product_main_loop 方法**
- [ ] **Step 3: 添加 _load_intent_from_mcp 方法**
- [ ] **Step 4: 添加 _product_llm_first_response 方法**
- [ ] **Step 5: 添加 _product_llm_with_tools 方法**
- [ ] **Step 6: 添加 _parse_product_action 方法**
- [ ] **Step 7: 添加 _save_product_session 方法**

- [ ] **Step 8: 编译验证**

```bash
python -c "import ast; ast.parse(open('core/product_agent.py').read()); print('OK')"
```

- [ ] **Step 9: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/core/product_agent.py
git commit -m "feat(product_agent): 新增 product_main_loop LLM 对话循环

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Session 状态字段扩展

**Files:**
- Modify: `mcp_servers/mcp_setup.py:52-65`

**Interfaces:**
- Consumes: 无
- Produces: session dict 包含 `stage` / `product_history` / `last_action` 字段

```python
# ── 替换 get_or_create_session ──

def get_or_create_session(sid: str) -> dict:
    if sid not in sessions:
        product = ProductAgent(llm=llm, mcp=product_mcp, config=cfg)
        guide = ShoppingGuideAgent(llm=llm, mcp=guide_mcp, config=cfg)
        guide.start_session(sid)
        sessions[sid] = {
            "guide": guide,
            "product": product,
            "stage": "guide",           # "guide" | "product"
            "product_history": [],      # 产品阶段对话记录
            "last_action": None,        # 最近产品 agent 动作
            "candidates": [],
            "last_intent": None,
        }
    return sessions[sid]
```

注意：`ShoppingGuideAgent` 不再传 `product_agent` 参数（Task 1 已经移除了对它的依赖）。

- [ ] **Step 1: 替换 get_or_create_session**

```python
def get_or_create_session(sid: str) -> dict:
    if sid not in sessions:
        product = ProductAgent(llm=llm, mcp=product_mcp, config=cfg)
        guide = ShoppingGuideAgent(llm=llm, mcp=guide_mcp, config=cfg)
        guide.start_session(sid)
        sessions[sid] = {
            "guide": guide,
            "product": product,
            "stage": "guide",
            "product_history": [],
            "last_action": None,
            "candidates": [],
            "last_intent": None,
        }
    return sessions[sid]
```

- [ ] **Step 2: 编译验证**

```bash
python -c "import ast; ast.parse(open('mcp_servers/mcp_setup.py').read()); print('OK')"
```

- [ ] **Step 3: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/mcp_servers/mcp_setup.py
git commit -m "feat(session): 扩展 session dict 增加 stage/product_history 字段

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: gRPC Server Stage Routing

**Files:**
- Modify: `grpc_server.py:76-128`（ChatStream 方法）

**Interfaces:**
- Consumes: `session["stage"]`, `session["guide"]`, `session["product"]`（Task 4）
- Produces: 路由后的回复给前端

```python
# ── 替换 ChatStream 核心逻辑 ──

async def ChatStream(self, request, context):
    _ensure_initialized()
    msg = request.message
    sid = request.session_id or f"sess_{int(time.time())}"
    print(f"[request] ChatStream  sid={sid[:20]}  stage={...}  msg={msg[:40]}...", flush=True)

    sess = _get_or_create_session(sid)
    guide = sess["guide"]
    product = sess["product"]
    t0 = time.time()

    yield shopping_pb2.ShoppingEvent(meta=shopping_pb2.MetaEvent(
        session_id=sid, stage=sess["stage"], ready=False))

    try:
        if sess["stage"] == "guide":
            # ── 导购阶段 ──
            result = guide.analyze_l1([{"role": "user", "content": msg}])

            if result.get("final") is True:
                # 切换到产品阶段
                print(f"[request] final detected → switch to product stage", flush=True)
                sess["stage"] = "product"
                sess["last_action"] = "final"
                result = product.product_main_loop(
                    session_id=sid, first_call=True)

        elif sess["stage"] == "product":
            # ── 产品阶段 ──
            sess["last_action"] = "product_message"
            product_history = sess.get("product_history", [])
            result = product.product_main_loop(
                session_id=sid,
                user_message=msg,
                history=product_history,
            )
            # 同步历史到 session（product_main_loop 内部也维护了一份）
            sess["product_history"] = product._product_history

    except Exception as e:
        print(f"[agent] ChatStream 失败: {e}", flush=True)
        yield shopping_pb2.ShoppingEvent(
            error=shopping_pb2.ErrorEvent(message=str(e)))
        return

    t1 = time.time()
    elapsed_ms = int((t1 - t0) * 1000)
    
    reply = result.get("reply", "")
    candidates = result.get("candidates", [])
    
    print(f"[request] ← reply  {len(reply)}B  {elapsed_ms}ms  candidates={len(candidates)}", flush=True)

    # 组装候选产品列表
    done_candidates = []
    for c in candidates[:5]:
        done_candidates.append(shopping_pb2.Candidate(
            name=str(c.get("name", c.get("skuName", ""))),
            price=str(c.get("price", c.get("current_price", "0"))),
            rating=str(c.get("rating", "0")),
        ))

    if reply:
        yield shopping_pb2.ShoppingEvent(
            token=shopping_pb2.TokenEvent(token=reply))
        yield shopping_pb2.ShoppingEvent(done=shopping_pb2.DoneEvent(
            full_text=reply, candidates=done_candidates))
    else:
        yield shopping_pb2.ShoppingEvent(done=shopping_pb2.DoneEvent(
            full_text="抱歉，我没有理解你的需求，请重新说一遍。"))
```

- [ ] **Step 1: 替换 ChatStream 核心逻辑**
- [ ] **Step 2: 编译验证**

```bash
python -c "import ast; ast.parse(open('grpc_server.py').read()); print('OK')"
```

- [ ] **Step 3: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/grpc_server.py
git commit -m "feat(grpc_server): 添加 stage routing，final 后移交 product agent

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Web Server Stage Routing

**Files:**
- Modify: `web_server.py:105-168`（`_handle_chat_stream` 方法）

**Interfaces:**
- Consumes: 同 Task 5，HTTP SSE 版本
- Produces: SSE 流式回复

```python
# ── 替换 _handle_chat_stream 核心逻辑 ──

def _handle_chat_stream(self, body):
    msg = body.get("message", "")
    sid = body.get("session_id", "")
    print(f"[request] POST /api/chat  sid={sid[:20] if sid else 'new'}  msg={msg[:40]}...", flush=True)
    sid = sid or f"sess_{int(time.time())}"
    sess = _get_or_create_session(sid)
    guide = sess["guide"]
    product = sess["product"]
    t0 = time.time()

    # ── 发送 SSE header ──
    self.send_response(200)
    self.send_header("Content-Type", "text/event-stream; charset=utf-8")
    self.send_header("Cache-Control", "no-cache")
    self.send_header("Access-Control-Allow-Origin", "*")
    self.end_headers()

    meta = json.dumps({"session_id": sid, "stage": sess["stage"], "ready": False}, ensure_ascii=False)
    self.wfile.write(f"data: {meta}\n\n".encode())
    self.wfile.flush()

    try:
        if sess["stage"] == "guide":
            result = guide.analyze_l1([{"role": "user", "content": msg}])
            
            if result.get("final") is True:
                print(f"[request] final detected → switch to product stage", flush=True)
                sess["stage"] = "product"
                sess["last_action"] = "final"
                result = product.product_main_loop(
                    session_id=sid, first_call=True)
                    
        elif sess["stage"] == "product":
            sess["last_action"] = "product_message"
            product_history = sess.get("product_history", [])
            result = product.product_main_loop(
                session_id=sid,
                user_message=msg,
                history=product_history,
            )
            sess["product_history"] = product._product_history

    except Exception as e:
        print(f"[agent] _handle_chat_stream 失败: {e}", flush=True)
        err = json.dumps({"error": str(e)}, ensure_ascii=False)
        self.wfile.write(f"data: {err}\n\n".encode())
        self.wfile.flush()
        return

    t1 = time.time()
    reply = result.get("reply", "")
    candidates = result.get("candidates", [])
    print(f"[request] ← reply  {len(reply)}B  {((t1-t0)*1000):.0f}ms  candidates={len(candidates)}", flush=True)

    # 分批推送回复文本
    if reply:
        chunk_size = 4
        for i in range(0, len(reply), chunk_size):
            chunk = json.dumps({"token": reply[i:i+chunk_size]}, ensure_ascii=False)
            self.wfile.write(f"data: {chunk}\n\n".encode())
            self.wfile.flush()

    # 候选产品
    done_candidates = []
    for c in candidates[:5]:
        done_candidates.append({
            "name": str(c.get("name", c.get("skuName", ""))),
            "price": str(c.get("price", c.get("current_price", "0"))),
            "rating": str(c.get("rating", "0")),
        })

    done_data = json.dumps({
        "done": True, "full_text": reply or "抱歉，我没有理解你的需求，请重新说一遍。",
        "stage": sess["stage"], "ready": True, "candidates": done_candidates,
    }, ensure_ascii=False)
    self.wfile.write(f"data: {done_data}\n\n".encode())
    self.wfile.flush()
```

- [ ] **Step 1: 替换 _handle_chat_stream 方法**
- [ ] **Step 2: 编译验证**

```bash
python -c "import ast; ast.parse(open('web_server.py').read()); print('OK')"
```

- [ ] **Step 3: 提交**

```bash
git add examples/python_services/ai-agent-compare-values/web_server.py
git commit -m "feat(web_server): 添加 stage routing，final 后移交 product agent

Co-Authored-By: Claude <noreply@anthropic.com>"
```

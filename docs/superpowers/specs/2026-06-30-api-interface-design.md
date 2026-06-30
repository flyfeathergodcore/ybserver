# API Interface Documentation — Design

## Goal

撰写一份面向框架使用者的技术接口文档（非系统描述），覆盖开发者编写 Handler、注册路由、配置 Middleware、启动服务器所需的全部公有接口。

## Scope

只覆盖 **server 层开发者直接使用的接口**，不涉及内部实现（SessionRegion、Parser、RingBuffer 等）：

| 覆盖 | 不覆盖 |
|------|--------|
| Config、Router、RequestHandler | SessionRegion / 内存池 |
| Context、Response | nghttp2 帧处理 |
| Middleware | 延迟桶百分位计算 |
| MetricsCollector（公有方法） | 各 Session 内部状态机 |
| ReverseProxy / UpstreamPool | FileCache 内部实现 |

## Document Structure（已确认）

### 上卷：Quick Start（按场景，面向新手）

1. **最小服务器** — 从 main() 起步，Config → Router → Middleware → MultiServer 组装顺序
2. **路由注册** — Add() 路径语法、前缀匹配规则
3. **编写 Handler** — 继承 RequestHandler，实现 Handle()，Context 读请求、Response 构建响应
4. **异步 Handler** — HandleAsync() + IsAsync()=true，适合 I/O 场景
5. **反向代理** — UpstreamPool + ReverseProxy，YAML 配置
6. **Middleware** — HandlePre() / HandlePost()，CORS/日志/Metrics 三内置示例
7. **Metrics 监控** — /metrics.json、SSE 流、Dashboard 图表对应
8. **配置详解** — config.yaml 全字段 + 启动参数

### 下卷：API Reference（按类，面向查阅）

| 章 | 类 | 方法 |
|----|-----|------|
| 9 | `Router` | `Add()`, `Match()`, `Get/Post/Put/Delete()` |
| 10 | `RequestHandler` | `Handle()`, `HandleAsync()`, `IsAsync()` |
| 11 | `Context` | `Method()`, `Path()`, `Header()`, `HeaderAt()`, `Body()`, `IsHttp2()` |
| 12 | `Response` | 构造器, `None()`, `Raw()`, `Error()`, `Header()`, `EndHeaders()`, `Body()`, `BodyFile()` |
| 13 | `Middleware` | `HandlePre()`, `HandlePost()`, `GetType()` |
| 14 | `MetricsCollector` | `OnRequest()`, `Flush()`, `RenderMetricsJson()`, `RenderLatestSnapshot()` |

## Format Per Entry（API Reference 部分）

每个方法按以下模板：

```
### `ReturnType MethodName(ParamType param1, ParamType param2)`

**说明：** 一句话描述。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| param1 | ParamType | xxx |

**返回值：** ReturnType — 描述。

**注意：** 可选的前置条件 / 线程安全 / 生命周期说明。
```

## Output

- 文件路径：`docs/api-interface.md`
- 格式：GitHub Flavored Markdown
- 语言：中文

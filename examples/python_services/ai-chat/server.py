"""ai-chat gRPC server — AI Chat via litellm streaming (MySQL 持久化)"""
import asyncio, os
import grpc
from grpc.aio import server as grpc_server
import litellm
import mysql.connector
from mysql.connector.aio import connect as async_connect
import chat_pb2, chat_pb2_grpc

# ── MySQL 连接配置（从环境变量读取）──
_MYSQL_CONFIG = {
    "host":     os.getenv("MYSQL_HOST", "mysql"),
    "port":     int(os.getenv("MYSQL_PORT", "3306")),
    "user":     os.getenv("MYSQL_USER", "webcpp"),
    "password": os.getenv("MYSQL_PASSWORD", "webcpp123"),
    "database": os.getenv("MYSQL_DATABASE", "webcpp"),
}

def _model_base_url(model: str) -> str | None:
    if model.startswith("ollama/"):
        url = os.getenv("OLLAMA_BASE_URL")
        if url: return url
    key = model.upper()
    for ch in "-./: ":
        key = key.replace(ch, "_")
    key += "_BASE_URL"
    return os.getenv(key)

_DEFAULT_MODEL       = os.getenv("LLM_MODEL", "gpt-4o-mini")
_DEFAULT_MAX_TOKENS  = int(os.getenv("LLM_MAX_TOKENS", "2048"))
_DEFAULT_TEMPERATURE = float(os.getenv("LLM_TEMPERATURE", "0.7"))
_SYSTEM_PROMPT       = os.getenv("SYSTEM_PROMPT", "You are a helpful AI assistant.")

class SessionManager:
    def __init__(self):
        self._pool: dict[str, list[dict]] = {}

    async def _conn(self):
        return await async_connect(**_MYSQL_CONFIG)

    async def get_history(self, sid: str) -> list[dict]:
        """从 MySQL 加载会话历史"""
        try:
            cnx = await self._conn()
            cursor = await cnx.cursor()
            await cursor.execute(
                "SELECT role, content FROM chat_messages "
                "WHERE session_id = %s ORDER BY created_at",
                (sid,))
            rows = await cursor.fetchall()
            await cursor.close()
            await cnx.close()
            return [{"role": r[0], "content": r[1]} for r in rows]
        except Exception as e:
            print(f"[ai-chat] MySQL get_history 失败: {e}")
            return []

    async def append(self, sid: str, role: str, content: str):
        """写入 MySQL"""
        try:
            cnx = await self._conn()
            cursor = await cnx.cursor()
            await cursor.execute(
                "INSERT INTO chat_messages (session_id, role, content) "
                "VALUES (%s, %s, %s)",
                (sid, role, content))
            await cnx.commit()
            await cursor.close()
            await cnx.close()
        except Exception as e:
            print(f"[ai-chat] MySQL append 失败: {e}")

class ChatService(chat_pb2_grpc.ChatServiceServicer):
    def __init__(self, mgr: SessionManager):
        self.sessions = mgr

    async def ChatStream(self, request_iterator, context):
        # ── 读取第一条消息 ──
        try:
            first = await request_iterator.__anext__()
        except StopAsyncIteration:
            return

        req = first.chat_request
        sid = req.session_id

        # ── 解析模型配置（使用 proto 值，回退到环境变量默认值）──
        model       = req.config.model       or _DEFAULT_MODEL
        max_tokens  = req.config.max_tokens  or _DEFAULT_MAX_TOKENS
        temperature = req.config.temperature or _DEFAULT_TEMPERATURE

        # ── 根据模型名动态获取 base_url ──
        # 例如 model="qwen" → 查找 QWEN_BASE_URL
        base_url = _model_base_url(model)

        # ── 构造上下文消息列表（从 MySQL 加载历史）──
        messages = [{"role": "system", "content": _SYSTEM_PROMPT}]
        history = await self.sessions.get_history(sid)
        messages.extend(history)
        messages.append({"role": "user", "content": req.user_message})

        try:
            response = await litellm.acompletion(
                model=model,
                messages=messages,
                stream=True,
                base_url=base_url,          # None 时 litellm 使用默认端点
                max_tokens=max_tokens,
                temperature=temperature,
            )

            full_content = ""
            async for chunk in response:
                # ── 非阻塞检查取消信号 ──
                try:
                    cancel_msg = await asyncio.wait_for(
                        request_iterator.__anext__(), timeout=0.001)
                    if cancel_msg.HasField("cancel"):
                        await response.aclose()
                        return
                except (StopAsyncIteration, asyncio.TimeoutError):
                    pass

                delta = chunk.choices[0].delta
                if delta and delta.content:
                    full_content += delta.content
                    yield chat_pb2.ChatServerMessage(token=delta.content)

            # ── 保存历史到 MySQL ──
            await self.sessions.append(sid, "user", req.user_message)
            await self.sessions.append(sid, "assistant", full_content)

            yield chat_pb2.ChatServerMessage(finish=chat_pb2.STOP)

        except litellm.exceptions.AuthenticationError as e:
            yield chat_pb2.ChatServerMessage(error=f"auth error: {e}")
        except litellm.exceptions.RateLimitError as e:
            yield chat_pb2.ChatServerMessage(error=f"rate limit: {e}")
        except litellm.exceptions.ContextWindowExceededError as e:
            yield chat_pb2.ChatServerMessage(error=f"context too long: {e}")
        except Exception as e:
            yield chat_pb2.ChatServerMessage(error=str(e))

async def main():
    port = int(os.getenv("GRPC_PORT", "50051"))
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    chat_pb2_grpc.add_ChatServiceServicer_to_server(
        ChatService(SessionManager()), server)
    print(f"[ai-chat] starting on :{port}, default model={_DEFAULT_MODEL}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())

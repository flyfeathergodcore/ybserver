"""ai-chat gRPC server — AI Chat via litellm streaming"""
import asyncio, os
from typing import AsyncIterator
import grpc
from grpc.aio import server as grpc_server
import litellm
import chat_pb2, chat_pb2_grpc

class SessionManager:
    def __init__(self):
        self._sessions: dict[str, list[dict]] = {}
    def get_history(self, sid: str) -> list[dict]:
        return self._sessions.get(sid, [])
    def append(self, sid: str, role: str, content: str):
        self._sessions.setdefault(sid, []).append({"role": role, "content": content})

class ChatService(chat_pb2_grpc.ChatServiceServicer):
    def __init__(self, mgr: SessionManager):
        self.sessions = mgr
        self.model = os.getenv("LLM_MODEL", "gpt-4o-mini")

    async def ChatStream(self, request_iterator, context):
        try:
            first = await request_iterator.__anext__()
        except StopAsyncIteration:
            return

        req = first.chat_request
        sid = req.session_id
        messages = [{"role": "system", "content": "You are a helpful AI assistant."}]
        messages.extend(self.sessions.get_history(sid))
        messages.append({"role": "user", "content": req.user_message})

        try:
            response = await litellm.acompletion(
                model=self.model, messages=messages, stream=True,
                max_tokens=req.config.max_tokens or 2048,
                temperature=req.config.temperature or 0.7,
            )
            full_content = ""
            async for chunk in response:
                try:
                    cancel_msg = await asyncio.wait_for(
                        request_iterator.__anext__(), timeout=0.001)
                    if cancel_msg.HasField("cancel"):
                        response.close()
                        break
                except (StopAsyncIteration, asyncio.TimeoutError):
                    pass
                delta = chunk.choices[0].delta
                if delta and delta.content:
                    full_content += delta.content
                    yield chat_pb2.ChatServerMessage(token=delta.content)

            self.sessions.append(sid, "user", req.user_message)
            self.sessions.append(sid, "assistant", full_content)
            yield chat_pb2.ChatServerMessage(finish=chat_pb2.STOP)
        except Exception as e:
            yield chat_pb2.ChatServerMessage(error=str(e))

async def main():
    port = int(os.getenv("GRPC_PORT", "50051"))
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    chat_pb2_grpc.add_ChatServiceServicer_to_server(ChatService(SessionManager()), server)
    print(f"[ai-chat] starting on port {port}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())

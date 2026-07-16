FROM python:3.11-slim

WORKDIR /app

# 清空宿主机注入的代理
ENV HTTP_PROXY=""
ENV HTTPS_PROXY=""
ENV http_proxy=""
ENV https_proxy=""
ENV NO_PROXY="*"
ENV no_proxy="*"

COPY python_services/ai-chat/requirements.txt .
RUN pip install --no-cache-dir -i https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple \
    -r requirements.txt

# 编译 proto
COPY proto/ /app/proto/
RUN python -m grpc_tools.protoc \
    -I/app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/chat.proto

COPY python_services/ai-chat/server.py .

ENV GRPC_PORT=50051
EXPOSE $GRPC_PORT

CMD ["python", "server.py"]

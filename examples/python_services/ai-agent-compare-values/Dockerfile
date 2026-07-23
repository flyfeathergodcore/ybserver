FROM python:3.13-slim

WORKDIR /app

# ═══════════════════════════════════════════════════════════════
# 清空宿主机注入的代理
# ═══════════════════════════════════════════════════════════════
ENV HTTP_PROXY=""
ENV HTTPS_PROXY=""
ENV http_proxy=""
ENV https_proxy=""
ENV NO_PROXY="*"
ENV no_proxy="*"

# Python 依赖（清华 pypi 源）
RUN pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple/ \
    && pip install --no-cache-dir --progress-bar on \
    pymysql mysql-connector-python neo4j qdrant-client redis pyyaml httpx mcp python-dotenv \
    grpcio grpcio-tools openai python-statemachine

# 编译 Python gRPC 桩代码
COPY proto/shopping.proto /app/proto/shopping.proto
RUN python -m grpc_tools.protoc \
    -I/app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/shopping.proto

# 复制源码
COPY python_services/ai-agent-compare-values/ /app/

# 暴露端口
EXPOSE 50054

# 启动入口（gRPC 服务）
CMD ["python", "-u", "run.py"]

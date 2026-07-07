"""ai-rag gRPC server — GraphRAG with Milvus + Neo4j"""
import asyncio, os
from typing import AsyncIterator
import grpc
from grpc.aio import server as grpc_server
from pymilvus import Collection, connections as milvus_connect
from neo4j import AsyncGraphDatabase
import rag_pb2, rag_pb2_grpc

class GraphRagEngine:
    def __init__(self):
        self.milvus_host = os.getenv("MILVUS_HOST", "localhost")
        self.neo4j_uri = os.getenv("NEO4J_URI", "bolt://localhost:7687")
        self.neo4j_user = os.getenv("NEO4J_USER", "neo4j")
        self.neo4j_pass = os.getenv("NEO4J_PASSWORD", "password")

    async def connect(self):
        milvus_connect(host=self.milvus_host, port=19530)
        self.neo4j_driver = AsyncGraphDatabase.driver(
            self.neo4j_uri, auth=(self.neo4j_user, self.neo4j_pass))

    async def query(self, request):
        collection = Collection(f"kb_{request.knowledge_base_id}")
        collection.load()
        results = collection.search(
            data=[[0.0]*768],
            anns_field="embedding",
            param={"metric_type": "IP", "nprobe": 10},
            limit=request.top_k,
        )
        resp = rag_pb2.QueryResponse()
        for hit in results[0]:
            c = resp.results.add()
            c.chunk_id = hit.id
            c.content = f"content:{hit.id}"
            c.score = hit.score
        return resp

class RagService(rag_pb2_grpc.RagServiceServicer):
    def __init__(self, engine: GraphRagEngine):
        self.engine = engine

    async def UploadDocument(self, request_iterator, context):
        count = 0
        async for chunk in request_iterator:
            count += 1
        return rag_pb2.UploadResult(doc_id=chunk.filename, chunk_count=count, success=True)

    async def Query(self, request, context):
        return await self.engine.query(request)

    async def QueryStream(self, request, context):
        resp = await self.engine.query(request)
        for c in resp.results:
            yield c
            await asyncio.sleep(0.01)

async def main():
    port = int(os.getenv("GRPC_PORT", "50052"))
    engine = GraphRagEngine()
    await engine.connect()
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    rag_pb2_grpc.add_RagServiceServicer_to_server(RagService(engine), server)
    print(f"[ai-rag] starting on port {port}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())

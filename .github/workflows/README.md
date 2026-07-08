# GitHub Actions 工作流说明

本项目使用 GitHub Actions 实现完整的 CI/CD 流程。

## 📋 工作流概览

### 1. CI（持续集成）- `ci.yml`
**触发条件：** Push 到 main 分支或 PR

**功能：**
- 安装依赖（Asio、OpenSSL、SQLite、yaml-cpp、nghttp2、liburing）
- 构建项目
- 运行热插拔测试
- 构建并推送 Docker 镜像到 GHCR
- 自动部署到生产环境（self-hosted runner）

**状态：** [![CI](https://github.com/$REPO/actions/workflows/ci.yml/badge.svg)](https://github.com/$REPO/actions/workflows/ci.yml)

---

### 2. RPC 模块测试 - `rpc-tests.yml`
**触发条件：** 修改 `rpc/**` 或 `tests/rpc_test.cpp`

**功能：**
- 独立测试 RPC 模块功能
- 验证 ServiceDiscovery、LoadBalancer、RetryPolicy
- 检查接口完整性

**测试覆盖：**
- ✅ ServiceInstance、RpcError、RpcCallOptions 类型定义
- ✅ 负载均衡算法（最少连接）
- ✅ 重试策略（状态码驱动）

---

### 3. 多架构 Docker 构建 - `docker-multiarch.yml`
**触发条件：** Push、PR、Tag 推送

**功能：**
- 构建 AMD64 和 ARM64 架构镜像
- **生产环境：** AMD64/x64 平台
- **测试环境：** ARM64（本地 Docker 测试）
- 自动推送到 GHCR
- 使用 GitHub Actions Cache 加速构建
- 支持语义化版本标签

**架构说明：**
- 主架构：AMD64（生产部署）
- 测试架构：ARM64（本地开发测试）

**镜像标签规则：**
- `main` 分支 → `latest`
- `v1.2.3` tag → `v1.2.3`, `v1.2`, `v1`, `latest`
- PR → `pr-123`

---

### 4. 性能基准测试 - `benchmark.yml`
**触发条件：** Push、PR、每周一凌晨 2 点

**功能：**
- HTTP/1.1 吞吐量测试（wrk）
- HTTP/2 吞吐量测试（hey）
- RPC 模块性能测试
- Valgrind 内存泄漏检查
- 生成性能报告（上传为 Artifact）

**测试指标：**
- QPS（每秒请求数）
- 延迟分布（P50、P90、P99）
- 内存泄漏检测

---

### 5. 安全扫描 - `security.yml`
**触发条件：** Push、PR、每天凌晨 3 点

**功能：**

#### 5.1 CodeQL 静态分析
- 检测安全漏洞和代码质量问题
- 查询集：`security-and-quality`

#### 5.2 依赖漏洞扫描
- Trivy 扫描 Docker 镜像
- 检查 HIGH/CRITICAL 级别漏洞

#### 5.3 密钥泄露扫描
- TruffleHog 扫描代码历史
- 防止敏感信息泄露

#### 5.4 静态代码分析
- CPPCheck：全面代码检查
- Clang-Tidy：性能和可读性分析

---

### 6. 发布流程 - `release.yml`
**触发条件：** 推送 `v*` 标签（如 `v1.0.0`）

**功能：**

#### 6.1 创建 Release
- 自动生成变更日志
- 创建 GitHub Release 页面
- 包含使用文档和 Docker 命令

#### 6.2 构建发布镜像
- 多架构镜像（AMD64 + ARM64）
- 推送到 GHCR
- 标记为 latest

#### 6.3 生产部署
- 自动部署到生产环境
- 备份当前版本
- 健康检查
- 清理旧镜像（保留最近 3 个）

---

## 🚀 使用指南

### 本地测试

```bash
# 运行所有测试
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/rpc_test

# 运行性能基准测试
wrk -t4 -c100 -d30s --latency https://localhost:8443/
```

### 发布新版本

```bash
# 1. 更新版本号（如 v1.2.3）
git tag -a v1.2.3 -m "Release v1.2.3"

# 2. 推送标签触发自动发布
git push origin v1.2.3

# 3. GitHub Actions 自动执行：
#    - 生成变更日志
#    - 构建多架构镜像
#    - 创建 Release
#    - 部署到生产环境
```

### 手动触发工作流

在 GitHub Actions 页面可以手动触发部分工作流：
- Benchmark（性能测试）
- Security Scan（安全扫描）

---

## 📊 工作流状态

| 工作流 | 状态 | 说明 |
|--------|------|------|
| CI | ![CI Status](badge) | 主 CI 流程 |
| RPC Tests | ![RPC Tests](badge) | RPC 模块测试 |
| Multi-Arch Build | ![Multi-Arch](badge) | 多架构构建 |
| Benchmark | ![Benchmark](badge) | 性能基准测试 |
| Security | ![Security](badge) | 安全扫描 |
| Release | ![Release](badge) | 发布流程 |

---

## 🔧 配置要求

### Secrets 配置

在 GitHub 仓库设置中配置以下 Secrets：

- `GITHUB_TOKEN`（自动提供）- GHCR 推送权限

### Self-Hosted Runner（生产部署）

需要配置 self-hosted runner，路径：`/home/ubuntu/webcpp`

要求：
- Docker 和 Docker Compose 已安装
- 有访问 GHCR 的权限

---

## 📝 最佳实践

1. **提交前本地测试**
   ```bash
   ./build/rpc_test  # 快速验证
   ```

2. **PR 合并前确保 CI 通过**
   - 所有测试通过
   - 无安全漏洞
   - 代码覆盖率达标

3. **发布版本前运行完整测试**
   ```bash
   # 手动触发 Benchmark 和 Security 工作流
   ```

4. **版本号遵循语义化版本规范**
   - `v1.0.0` - 主版本（不兼容的 API 修改）
   - `v1.1.0` - 次版本（向下兼容的功能性新增）
   - `v1.1.1` - 修订版本（向下兼容的问题修正）

---

## 🐛 故障排查

### CI 构建失败

1. 检查依赖版本是否匹配
2. 查看具体失败步骤的日志
3. 本地复现问题

### Docker 镜像推送失败

1. 检查 GITHUB_TOKEN 权限
2. 确认 GHCR 访问正常
3. 查看网络连接

### 生产部署失败

1. 检查 self-hosted runner 状态
2. 确认 Docker 服务运行正常
3. 查看容器日志：`docker logs <container>`

---

## 📚 参考资料

- [GitHub Actions 文档](https://docs.github.com/actions)
- [Docker Buildx 多架构构建](https://docs.docker.com/buildx/working-with-buildx/)
- [CodeQL 查询](https://codeql.github.com/docs/)
- [Trivy 漏洞扫描](https://aquasecurity.github.io/trivy/)

# R09 稳固、测试与发布

## 目标

在 M0-M4 功能完成后，做确定性、性能、安全、稳定性、文档和发布收尾。

## 前置依赖

R00-R08。

## 范围

- 全链路故障注入/压力/长跑。
- Fuzz/ASan/TSan/UBSan。
- 性能基准与帧预算。
- 安全审查。
- 文档/示例/发布包。

## 任务清单

### 稳定性

- [x] 1h+ 加载/卸载/下载/热更循环，无泄漏/崩溃（`scripts/soak-r09.ps1`：1103 轮 / 60m01s / 4 stress cases per round / 0 failures）。
- [x] 随机 Handle 释放/依赖链/取消/重入（并发压力测试；完整依赖链随机图延后）。
- [x] LoadingLock/UnloadAll/DestroyPackage 压力。
- [x] 缓存损坏/临时文件残留（损坏、超大 `.part`、可续传失败、孤儿临时文件清理）。
- [x] 磁盘满恢复注入（`FlakyCommitSandbox` 模拟 commit ENOSPC；重试、最终失败、恢复下载）。

### Fuzz

- [x] PRCA/PRCL/PRCM fuzz target（独立 target + golden corpus smoke）。
- [x] FlatBuffers/BlobTable 边界（现有对象 metadata/BlobTable 测试 + fuzz）。
- [x] `.parcel.json`/`.parcelmeta`/remote config 解析 fuzz（`parcel_fuzz_sidecar`：1801s / 12,304,085 runs / 0 artifacts）。
- [x] 至少 30 分钟无崩溃（Windows ClangCL libFuzzer：1801s / 222,311,162 runs / 0 artifacts）。

### Sanitizers

- [x] ASan（MSVC，全量测试通过）。
- [x] UBSan（ClangCL 17 + MSVC STL，223 cases / 6095 assertions；`scripts/run-ubsan.ps1`）。
- [ ] TSan（线程池/下载/调度，如可行）。
- [x] MSVC ASan（可选）。
- [x] 清零已知 ASan 问题。

### 性能

- [x] Manifest 解析时间/内存（10k/100k 资产，`parcel_bench`）。
- [x] Bundle 加载/解压吞吐；block size 基线（8 MiB/64 KiB LZ4）。
- [x] 主线程帧预算（1k 独立 Raw Bundle，4ms 分帧卸载；Debug p95 1.6ms，OS 离群点 5.2ms；`parcel_performance`）。
- [x] 下载并发（多 worker、跨 Downloader/按需共享同一 Bundle in-flight gate）。
- [x] WinHTTP loopback 连接句柄复用（session/connection pool；`[.winhttp]` 统计验证）。
- [ ] 真实 CDN/TLS/吞吐（loopback 已通过；真实 CDN 未跑）。
- [x] 缓存扫描启动时间（10k `.bundle` fixture，`parcel_bench`）。
- [x] 缓存批量校验（Metadata/Size/Content；1-32 worker 并发；损坏删除、缺失跳过、跨 Bundle 继续）。
- [x] 缓存批量校验吞吐（10k/1 KiB：串行 14.8s、8 worker 9.1s；`parcel_bench`）。
- [x] 记录基准和回归阈值（`docs/13-benchmarks.md`）。

### 安全

- [x] 路径穿越/zip bomb/block 大小/类型白名单（`test_security.cpp` + Go API 测试）。
- [x] Token/签名 URL/密钥轮换/日志脱敏。
- [x] 上传大小/类型/哈希校验。
- [x] 管理端 CSRF/权限/限流。
- [x] 依赖许可证审查（`THIRD_PARTY_NOTICES.md`；assimp/bgfx/entt 为宿主依赖）。

### 文档/发布

- [x] README/快速开始/CMake 构建/服务端部署。
- [x] 格式/资产/Scene schema 文档与实现一致（现有文档 + schema）。
- [x] 示例：纹理/文本全链路；FBX + `.parcel.json` 需 assimp 宿主验证。
- [x] API/错误码/版本兼容说明（`docs/14-versioning-compat.md`）。
- [x] 发布包脚本：静态库/CLI/服务端二进制/管理端静态文件；镜像仍需 Docker 验证。
- [x] 版本号/变更日志。

## 验收标准

- 长跑/压力无泄漏、无崩溃、无死锁。
- fuzz/sanitizer 通过。
- 性能达到首期目标，记录基准。
- 安全清单全部通过。
- 文档/示例可让新成员从零跑通。
- 发布包可独立构建/部署。

## 估算

约 3-4 周（1-2 人），与 M4 部分重叠。














































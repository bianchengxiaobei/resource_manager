# R07 热更新、CDN、鉴权、灰度

## 目标

实现完整热更链路与客户端分发能力：版本、Manifest、下载、续传、重试、缓存修复、
多 CDN、Token、签名 URL、灰度、回滚。

## 前置依赖

R05。

## 范围

- 版本/Manifest 获取与缓存。
- Downloader + DownloadScheduler。
- 按需下载/缓存修复/清理。
- `parcel_remote` 客户端接口。
- mock server 测试。

## 状态

主体完成。已实现 `parcel_runtime` 下载器、Sandbox 原子安装/修复、
`parcel_remote` Manifest 缓存/RemoteManifestService/CDN/Token/灰度、
Windows WinHTTP 后端与进程内 MockTransport 测试。

已补齐：

- `max_requests_per_frame` 以 `scheduler_frame_interval`（默认 16ms）调度窗口限流；默认 8、范围 1-32。
- watchdog 已实现为下载器级“无进度”计时器：transport 通过 `on_bytes_received` 上报进度，
  `should_abort` 中断停滞传输；WinHTTP 仍叠加接收超时兜底。
- 显式 Downloader 与按需下载共享按临时路径的 in-flight gate，等待方复用 owner 结果。
- `SignedUrlProvider` 通过服务端 `/api/v1/client/url` 换取签名 URL，缓存至过期，并在 403/410 后重新签名；客户端不持有 HMAC 密钥。

明确延期：

- 真实 CDN 边缘的签名 URL/鉴权流程（TLS/连接复用/吞吐和完整 Bundle 安装已通过
  `scripts/run-cdn-smoke.ps1`/`scripts/run-cdn-bundle-smoke.ps1`；真实 Parcel
  服务端的签名 URL 链路已由 `scripts/run-signed-url-smoke.ps1` 在 loopback HTTP
  上验证，包含签名下载、未签名/篡改/过期 403 和无 Token 401；CDN 边缘仍需宿主
  CDN 配置）。
- 系统级磁盘满未注入；写前 FreeSpace 与 commit 失败注入已覆盖。

边界：生产签名密钥只在服务端；客户端通过 `/api/v1/client/url` 消费签名 URL。

## 任务清单

### 版本/Manifest

- [x] RequestVersion（超时/错误；时间戳参数由 URL Provider 决定）。
- [x] Manifest hash + bytes 获取；缓存；SHA-256 校验。
- [x] 原子切换 active Manifest；旧操作固定旧 Manifest。
- [x] 版本回退/多版本缓存/清理。
- [x] 失败回退本地版本缓存（内置版本由 ResourcePackage 初始化提供）。

### Downloader

- [x] 下载列表：all/tags/locations；差异计算（排除 Builtin）。
- [x] 并发上限。
- [x] 每帧请求上限（`max_requests_per_frame` + scheduler frame；启动配额用例已覆盖）。
- [x] Range 续传 + 最小续传尺寸；不支持 Range 重下。
- [x] 重试策略：可重试错误/指数退避/抖动/最大次数。
- [x] watchdog 无进度超时（transport 进度回调 + abort；停滞/持续进度用例已覆盖）。
- [x] 暂停/恢复/取消；取消删除临时文件。
- [x] 原子安装：temp → size/SHA 校验 → rename。
- [x] 进度/计数/失败列表/RetryFailed/事件。
- [x] 按需下载与显式 Downloader 共享调度（in-flight gate；跨 Downloader/按需等待用例已覆盖）。

### 缓存修复/清理

- [x] 加载失败时验证缓存；损坏删除/重下。
- [x] ClearCache：全部/未使用/ByTags/ByLocations/旧版本。
- [x] VerifyCache/EnsureBundle。
- [x] 磁盘空间检查；临时文件残留清理。
- [x] footprint/旧版本清理（Bundle + Manifest 缓存）。

### Remote 客户端

- [x] `IRemoteConfigProvider`。
- [x] `IVersionResolver`：服务端决策 + 客户端分桶。
- [x] `IUrlProvider`：多 CDN 候选/回退。
- [x] `IAuthProvider`：Token/Header、401 刷新一次。
- [x] 签名 URL 消费/过期重签（`SignedUrlProvider`；服务端签名 URL + 403/410 重新签名）。
- [x] 灰度：百分比/白名单/平台/App 版本。
- [x] 回滚/离线缓存。
- [x] OpenAPI/JSON schema 对齐（R08）：`server/internal/api/openapi_test.go` 双向校验
  method/path，并核对主要 schema 顶层 JSON 字段；`openapi-typescript` 生成文件无
  diff，`vue-tsc`/Vite 构建通过。

### Mock Server/测试

- [x] 本地 HTTP loopback server smoke（`scripts/run-winhttp-smoke.ps1`；Python 静态服务 + WinHTTP 真实下载）。
- [x] 真实 CDN/TLS/吞吐 smoke（`scripts/run-cdn-smoke.ps1`；Cloudflare 8 MiB，证书链、连接复用和冷/热吞吐）。
- [x] 真实 CDN 完整 Bundle 安装 smoke（`scripts/run-cdn-bundle-smoke.ps1`；GitHub raw 328 B fixture，下载/校验/缓存安装）。
- [x] 真实服务端签名 URL 链路（`scripts/run-signed-url-smoke.ps1`：启动 `parcel-server`、创建包/版本/发布、签发设备 Token，再用 WinHTTP + `SignedUrlProvider` 下载；签名下载、未签名/篡改/过期 403、无 Token 401，32 assertions）。
- [x] MockTransport 覆盖 200/401/403/404/500、慢响应、传输失败。
- [x] Range/不支持 Range。
- [x] Token/签名 URL/灰度变体。
- [x] 集成测试：首装/增量/回滚策略/续传/损坏修复；磁盘满仅有写前检查。

## 验收标准

- 首次安装、增量更新、回滚、断点续传、损坏修复全流程通过（MockTransport；loopback
  WinHTTP 真实下载、真实 CDN/TLS 传输/完整 Bundle 安装和真实服务端签名 URL smoke
  通过；CDN 边缘签名配置待宿主）。
- 多 CDN 失败可切换；401 刷新一次；签名 URL 过期/403 可重新签名。
- 灰度百分比/白名单/平台/App 版本正确；回滚生效。
- 下载失败/进度/取消/暂停行为可预测。
- 缓存清理/校验不误删 Builtin/正在使用文件。

## 估算

约 3-4 周（1-2 人）。








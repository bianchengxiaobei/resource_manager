# R08 Go 服务端与 Vue 管理端

## 目标

实现单节点生产 MVP 服务端与管理端：

- 包/版本/渠道/灰度/回滚/审计。
- 文件分发/签名 URL/Range。
- Go + SQLite + 本地存储 + Docker。
- Vue 3 + TypeScript + Vite + Element Plus。

## 前置依赖

R07（客户端协议/配置 schema）；可并行开发。

## 状态

主体完成。已落地 Go 服务端、Vue 管理端、CLI、OpenAPI、Docker/nginx/备份脚本；
端到端 API 测试覆盖上传/发布/灰度/回滚/重命名/删除/签名 URL/Range/401/403/404/409/429/路径穿越。

明确延期：

- Docker 容器真实启动 smoke；当前环境未安装 Docker。Windows nginx 1.27.5 + 本地 CA
  签发证书（Go TLS probe 校验证书链和主机名）的 HTTP→HTTPS、静态 UI、登录和建包
  代理 smoke 已通过 `scripts/run-nginx-smoke.ps1`。
- 大规模并发/断点压力测试；当前覆盖 Range 与并发安全基础。

## 范围

- `parcel-server`、`parcel-admin`（Go）。
- SQLite/本地内容寻址存储。
- 管理端 Session+CSRF；客户端设备 Token/JWT；HMAC 签名 URL。
- 灰度/回滚/审计/指标。
- Vue 管理端。
- Docker/nginx/备份。

## 任务清单

### 服务端基础

- [x] Go module、目录、配置、日志（slog）。
- [x] SQLite schema + migrations。
- [x] 本地 blob 存储（SHA-256 内容寻址、原子写）。
- [x] 上传 API：multipart/流式、SHA-256 校验、大小限制。
- [x] 包/版本/渠道 CRUD，含包/版本重命名与破坏性删除（确认参数、引用保护、blob 清理）。
- [x] 发布/回滚。
- [x] 审计日志。
- [x] Prometheus `/metrics`、`/healthz`。
- [x] Dockerfile + docker-compose + nginx TLS + volume + backup 脚本。
- [x] Windows nginx TLS 代理 smoke（本地 CA 签发证书；HTTP→HTTPS、HTTPS
  `/healthz`、静态 UI、管理员登录/建包；Go probe 校验证书链；Docker 容器启动
  仍未验证）。
- [x] OpenAPI 契约测试：实际 method/path 与 `api/openapi.yaml` 双向一致，主要 schema
  JSON 字段与 Go tag 一致；TS schema 生成无 diff，`vue-tsc`/Vite 构建通过。

### 鉴权

- [x] 管理端用户名/密码（Argon2/bcrypt）+ Session Cookie + CSRF。
- [x] 管理 API Key（CLI）。
- [x] 客户端设备 Token/JWT 签发/校验/过期。
- [x] 文件 HMAC 签名 URL（expires/key_id/rotation）。
- [x] 限流/审计/敏感信息不落日志。
- [x] 路径穿越/上传校验。

### 灰度/版本

- [x] 灰度规则 CRUD（百分比/白名单/平台/App 版本/优先级）。
- [x] `/client/config` 版本决策：服务端优先 + 客户端分桶兜底。
- [x] 回滚：channel 指针切换，保留旧 artifact。
- [x] 强制更新/min app version。
- [x] 多 CDN 列表返回。
- [x] 灰度/下载/失败指标。

### 数据面

- [x] Manifest/Bundle 下载；Range/ETag/Cache-Control。
- [x] 签名 URL 校验。
- [x] 不可变缓存/内容寻址。
- [x] 大文件/断点/并发测试。

### Vue 管理端

- [x] Vue 3 + TS + Vite + Element Plus。
- [x] OpenAPI 生成 TS client。
- [x] 登录/登出/CSRF。
- [x] 包/版本列表；上传/发布/重命名/删除。
- [x] 渠道/灰度规则/回滚。
- [x] 审计/指标页面。
- [x] 上传进度/错误提示/权限。
- [x] Playwright smoke（`e2e/admin.spec.ts`：登录、创建包、上传两个版本、发布、回滚、创建/编辑/删除灰度规则、审计分页/指标页、登出；使用系统 Edge headless，`scripts/run-webui-smoke.ps1` 启动临时服务端）。

### CLI

- [x] `parcel-admin upload/publish/rollback/rename-package/delete-package/rename-version/delete-version/gray/gray-update/gray-delete/backup/restore`。
- [x] 退出码/日志/配置。

## 验收标准

- 单节点服务可 Docker 启动，健康检查/指标正常。
- 管理端完成登录→上传→发布→灰度→回滚闭环。
- 客户端可获取 config/version/manifest/bundle；签名 URL 生效/过期。
- 401/403/404/500、限流、路径穿越测试通过。
- 备份/恢复流程可用。
- OpenAPI 与实现一致。

## 估算

约 5-7 周（1-2 人，可与 C++ 并行）。



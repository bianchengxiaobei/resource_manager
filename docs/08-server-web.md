# 08 Go 服务端与 Vue 管理端

## 1. 服务端定位

单节点生产 MVP：

- 控制面：包/版本/渠道/灰度/回滚/审计。
- 数据面：Manifest/Bundle 下载、签名 URL、Range/ETag/缓存头。
- 存储：SQLite + 本地内容寻址文件；后续可换 PostgreSQL/S3。
- 部署：Docker + nginx TLS + volume + backup。

## 2. 技术栈

- Go 1.22+；`chi`/`gin`（推荐 chi）或标准库。
- SQLite：`modernc.org/sqlite`（纯 Go）或 `mattn/go-sqlite3`。
- Auth：`golang-jwt/jwt`、Argon2/bcrypt、HMAC-SHA256。
- OpenAPI 3.1；Go 使用 `oapi-codegen`。
- 指标：Prometheus；结构化日志（slog）。
- Vue 3 + TypeScript + Vite + Element Plus；OpenAPI 生成 TS client。

## 3. 数据模型（SQLite）

```text
users             管理员/操作员（id, username, password_hash, role）
sessions          管理端会话（id, user_id, csrf_token, expires_at）
packages          package（id, name, created_at）
versions          package_version（id, package_id, version, state, manifest_hash, note）
artifacts         files（id, version_id, sha256, size, path, kind）
channels          channel -> current_version（id, package_id, name, version_id）
gray_rules        （id, package_id, version_id, percentage, whitelist, platforms,
                    min_app_version, priority, enabled）
device_tokens     （id, player_id, token_hash, expires_at, created_at）
audit_logs        （id, user_id, action, target, detail, created_at）
```

- `artifacts` 内容寻址：`data/blobs/<sha256>`；DB 只存元数据。
- 上传时校验 SHA-256；先写临时文件再原子 rename。
- 版本状态：`draft → published → rolled_back`。
- 回滚只切 channel 指针，不删除旧 artifact。

## 4. API

### 客户端

```text
POST /api/v1/auth/device-token
GET  /api/v1/client/config?package=&channel=&player_id=&platform=&app_version=
GET  /api/v1/client/version?...
GET  /api/v1/client/url?path=/files/...   # 设备 JWT 换取签名 URL
GET  /files/{path}?expires=&key_id=&signature=
```

### 管理端

```text
POST /api/v1/admin/login
POST /api/v1/admin/logout
GET  /api/v1/admin/packages
POST /api/v1/admin/packages
POST /api/v1/admin/packages/{pkg}/versions
POST /api/v1/admin/packages/{pkg}/versions/{ver}/publish
POST /api/v1/admin/packages/{pkg}/rollback
GET  /api/v1/admin/packages/{pkg}/channels
PUT  /api/v1/admin/packages/{pkg}/channels/{channel}
GET  /api/v1/admin/packages/{pkg}/gray-rules
POST /api/v1/admin/packages/{pkg}/gray-rules
PUT  /api/v1/admin/packages/{pkg}/gray-rules/{id}
DELETE /api/v1/admin/packages/{pkg}/gray-rules/{id}
GET  /api/v1/admin/audit
GET  /healthz
GET  /metrics
```

- 所有响应 JSON；错误统一 `{code, message, request_id}`。
- 分页；上传支持 multipart/流式；大文件限制可配置。

## 5. 鉴权

- 管理端：用户名/密码（Argon2 或 bcrypt）+ Session Cookie（HttpOnly/Secure/SameSite）
  + CSRF Token（`X-CSRF-Token`）。
- 客户端：设备 Token / JWT；`POST /auth/device-token` 签发；过期时间可配置。
- 管理 API 同时支持 API Key（CLI）。
- 文件 URL：HMAC-SHA256(`path + expires + key_id`)；密钥轮换。
- 限流：登录、上传、下载；审计管理操作。
- 不记录 Token/签名/密码。

## 6. 灰度

- 规则字段：版本、百分比、白名单、平台、App 版本区间、优先级、启用。
- 服务端决策优先；客户端分桶兜底。
- 稳定分桶：`sha256(player_id + package + salt) % 10000 < percentage`。
- 规则按 priority 排序，第一个命中生效。
- 回滚：channel 指针切到旧版本；旧 artifact 保留。
- 强制更新：`min_app_version` 不满足时返回错误/提示。

## 7. 签名 URL / CDN

- 服务端作为 origin，返回签名 URL；可选外部 CDN 回源。
- 文件不可变、内容寻址；Cache-Control 长缓存。
- 支持 Range/ETag/Last-Modified。
- 签名 URL 过期时间可配置；过期后客户端重新请求 config。
- CDN 多域名由 config 返回；服务端不强制代理。

## 8. 部署

```text
Dockerfile:
  - Go build
  - Vue build
  - embed static files
docker-compose:
  - parcel-server
  - nginx (TLS)
  - volume: /data (SQLite + blobs)
```

- 环境变量：JWT secret、HMAC keys、DB path、storage root、admin bootstrap、listen address。
- 备份：SQLite + blobs 目录；提供 `parcel-admin backup` 或脚本。
- 健康检查、Prometheus 指标、结构化日志、审计。
- 升级：DB migration（golang-migrate 或内嵌 SQL）；启动时自动迁移（可配置）。

## 9. Vue 管理端

页面：

- 登录
- 包列表 / 新建包
- 版本列表 / 上传 / 发布
- 渠道配置
- 灰度规则（百分比/白名单/平台/App 版本）
- 回滚
- 审计日志
- 指标概览（下载/失败/灰度命中）

实现：

- Vue 3 + TypeScript + Vite + Element Plus。
- OpenAPI 生成 TS client；统一错误处理。
- Session Cookie + CSRF；登录后获取 CSRF Token。
- 上传：分片/进度/取消；校验 SHA-256（前端可算/服务端校验）。
- 权限：admin/operator 角色；按钮级权限。
- 构建产物由 Go `embed` 提供；开发模式 Vite proxy。

## 10. 测试

- 错误 JSON、HTTP 状态码、API/格式版本兼容策略见 `docs/14-versioning-compat.md`。

- Go：httptest + 临时 SQLite；上传/发布/回滚/灰度/签名 URL/401/403。
- 签名 URL 过期、密钥轮换、路径穿越、限流。
- Vue：Playwright 登录/上传/发布/回滚 smoke（可选）。
- 部署：docker-compose 启动、健康检查、备份恢复。
## 11. R08 当前实现

状态：主体完成。服务端、管理端、CLI、OpenAPI、Docker 部署文件已落地。

### 11.1 目录

```text
server/
  cmd/parcel-server/        HTTP 服务入口、healthcheck
  cmd/parcel-admin/         upload/publish/rollback/gray/backup CLI
  internal/api/             chi 风格标准库路由、鉴权、限流、文件/客户端/管理 API
  internal/store/           SQLite、migration、包/版本/渠道/灰度/审计
  internal/blob/            SHA-256 内容寻址 blob、原子 rename
  internal/secure/          bcrypt、JWT HS256、HMAC URL 签名、API Key hash
  internal/metrics/         轻量 Prometheus 文本指标
  internal/backup/          SQLite VACUUM INTO + blobs 备份
  internal/webui/           Vue 构建产物 embed
  web/                      Vue 3 + TypeScript + Vite + Element Plus
api/openapi.yaml            Go/Vue 共用契约
deploy/                     Dockerfile/compose/nginx/backup
```

### 11.2 构建与测试

```sh
cd server
go test ./...
go build ./cmd/parcel-server
go build ./cmd/parcel-admin

cd web
npm ci
npm run generate:api
npm run build
```

`web` 的构建产物写入 `server/internal/webui/dist`，随后由 Go `embed` 提供。
Docker 构建顺序为 Vue build → Go build → distroless 运行镜像。

### 11.3 数据与协议

- SQLite WAL，启动自动执行 `internal/store/migrations/*.sql`；并发写事务使用
  `BEGIN IMMEDIATE` + busy timeout。
- `manifest.hash` 与 `parcel_pack` 一致：SHA-256 覆盖 PRCM 160 字节头之后的 payload；服务端上传时同时校验 header 内的 `content_hash`。
- 数据面路径：
  - `/files/<package>/<version>/<package>.version`
  - `/files/<package>/<version>/<package>_<version>.hash`
  - `/files/<package>/<version>/<package>_<version>.bytes`
  - `/files/<package>/<version>/bundles/<bundle>`
- 文件使用 SHA-256 ETag、`Cache-Control: immutable`、Range/If-Range（`http.ServeContent`）。
- `/api/v1/client/config` 返回 R07 schema：`cdn` 为 `<CDN root>/files/<pkg>/<ver>/`，
  `sign_url` 为 `/api/v1/client/url`；
  `PARCEL_CDN_URLS` 是 CDN origin root（逗号分隔），未配置时使用 `PARCEL_PUBLIC_BASE_URL`。
- 签名 URL：`GET /api/v1/client/url?path=/files/...`（设备 JWT）返回
  `expires/key_id/signature`；签名覆盖 `key_id\npath\nexpires`。
  `PARCEL_REQUIRE_SIGNED_FILES=true` 时未签名下载返回 403；密钥可通过
  `PARCEL_HMAC_KEYS=old:secret,primary:secret` 轮换。
- 灰度：服务端按 priority 选版本；`percentage` 使用与 R07 完全一致的
  `sha256(player_id + package + salt)` 前 4 字节大端 `% 10000`。响应同时返回启用规则，
  供客户端离线/兜底使用。
- 回滚只切 channel 指针并保留旧版本 artifact；`previous_version` 从已发布时间取。
- 强制更新：`PARCEL_MIN_APP_VERSION` 不满足时配置返回 `force_update=true`；
  R07 客户端可据此提示升级。

### 11.4 鉴权与安全

- 管理端 bcrypt + Session Cookie（HttpOnly/SameSite=Lax，生产 Secure）+ CSRF。
- 管理员 API Key（`X-API-Key`）供 CLI 使用；API Key 只在创建时返回一次，存 SHA-256 hash。
- 客户端设备 JWT：`POST /api/v1/auth/device-token`，HS256、过期、jti、package/platform
  claim；签名 URL 与 `/client/url` 强制 Bearer。
- 登录、设备 Token、上传、下载有按 IP 的窗口限流；`X-Forwarded-For` 仅在
  `PARCEL_TRUST_PROXY=true` 时采信。
- 管理操作写审计日志；日志不记录密码、Token、签名、API Key。
- 路径穿越、multipart 文件名、包/版本名、blob 路径均做白名单校验。

### 11.5 管理端

- 登录/登出/CSRF；包列表/新建；版本列表/上传（进度）/发布；渠道选择；
  灰度规则创建/编辑/删除；回滚；审计分页；指标概览。
- 上传使用 `XMLHttpRequest` 显示进度，前后端均校验 manifest/header。
- OpenAPI `api/openapi.yaml` 通过 `openapi-typescript` 生成
  `server/web/src/api/schema.d.ts`。

### 11.6 运维

```sh
cd deploy
cp .env.example .env
# 配置证书到 certs/server.crt、certs/server.key
docker compose --env-file .env up -d --build
sh backup.sh
```

恢复（先停止服务）：

```sh
parcel-admin restore --backup backups/<timestamp> --db /data/parcel.db --blobs /data/blobs
```

Docker 镜像包含 `parcel-admin`；备份命令使用 SQLite `VACUUM INTO`，再复制不可变
blobs。恢复时停止服务，替换 `parcel.db` 与 `blobs` 后启动。

### 11.7 明确延后

- 真实 Docker/nginx TLS 启动 smoke（当前环境无 Docker）。
- Playwright UI smoke（可选）。
- 跨节点 PostgreSQL/S3、分布式锁、CDN 预热。
- 大规模并发/断点压力测试；当前 API 测试覆盖 Range、限流、签名、路径穿越。
- CLI `gray` 目前覆盖创建；更新/删除灰度规则走管理端或 API。








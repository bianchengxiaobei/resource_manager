# 14 API、错误码与版本兼容

## 1. 版本轴

| 对象 | 当前版本 | 兼容策略 |
|---|---|---|
| PRCA/PRCL/PRCM | 1 | 读取时拒绝未知版本；写入只产生当前版本 |
| PRCM flags/schema | 1 | 未知 flag 不影响基本读取；不兼容语义拒绝 |
| `.parcelmeta` | 1 | `version` 不匹配时拒绝，保留源文件 |
| `.parcel.json` Scene | 1 | `version` 不匹配时拒绝；未知字段按 schema 处理 |
| Pack JSON | 无版本字段 | 未知字段拒绝；新增字段保持可选 |
| 服务端 HTTP API | `/api/v1` | 路径版本化；不兼容变更进入 `/api/v2` |
| Client config/version JSON | `schema: 1` | 新增字段默认向后兼容；移除/改义需升 schema |
| Vue/OpenAPI | `api/openapi.yaml` | 生成类型随 API 提交；契约变更必须同步前端 |

## 2. HTTP 错误格式

所有 JSON 错误使用：

```json
{
  "code": "invalid_request",
  "message": "human readable",
  "request_id": "..."
}
```

常见状态码/错误码：

| HTTP | code | 含义 |
|---|---|---|
| 400 | `invalid_request` | 参数/JSON/路径/表单无效 |
| 401 | `unauthorized` | 缺少或无效 Session/设备 JWT |
| 403 | `forbidden` | CSRF/角色/签名/包不匹配 |
| 404 | `not_found` | 包、版本、文件或规则不存在 |
| 409 | `conflict` | 名称/版本冲突（实现相关） |
| 413 | `too_large` | 上传超过限制 |
| 429 | `rate_limited` | 触发限流，带 `Retry-After` |
| 500 | `internal` | 服务端内部错误，不返回敏感细节 |

`request_id` 用于日志关联，不包含用户/Token 信息。

## 3. 客户端兼容

- R07 `RemoteConfig` 只依赖 `schema`、`version`、`manifest_hash`、`cdn`、
  `auth`、`gray`、`rollback`；新增 `force_update` 等字段可忽略。
- Manifest hash 统一为 PRCM 160 字节头后 payload 的 SHA-256；客户端接受带/不带
  `sha256:` 前缀。
- Archive bundle 使用 Manifest asset flags bit 0 `ArchiveRaw` 和 PRCL `raw_files`；
  旧版 Runtime 不能读取 archive raw，需使用配套 Runtime。非 archive 包保持兼容。
- 文件路径约定为 `/files/<package>/<version>/...`；CDN 根地址可替换。
- 签名 URL 变更参数名或 canonical 字符串属于不兼容变更，必须升 API 版本。

## 4. 弃用流程

1. 文档标注 deprecated，并保留至少一个 minor 版本。
2. 服务端同时接受旧/新字段，响应只用新字段。
3. 管理端和 OpenAPI 先生成新类型。
4. 移除前在 `CHANGELOG.md` 记录，并提升 API 主版本。

## 5. 发布检查

- `VERSION`、CMake `project(VERSION)`、CHANGELOG 一致。
- OpenAPI、`server/web/src/api/schema.d.ts`、Go handler 同步。
- 格式/schema 变更更新 `docs/02`/`docs/03` 和 golden files。
- 服务端升级先备份 SQLite + blobs；回滚保留旧版本数据目录。

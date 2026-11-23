# 06 热更新、CDN、鉴权、灰度

## 1. 热更链路

```text
InitializePackage
  → RequestVersion
  → LoadManifest（缓存 + hash 校验）
  → CreateDownloader（差异计算）
  → Download（并发/续传/重试/校验/原子落盘）
  → LoadAsset（按需 Ensure/下载）
  → ClearCache / Verify / Repair
```

规则：

- 版本字符串按相等比较，不排序；允许回退。
- Manifest 缓存按 version + content hash；hash 不匹配重新下载。
- Manifest 原子切换；旧操作固定旧 Manifest。
- Bundle 内容寻址不可变；新旧版本 Bundle 可共存。
- 下载失败可回退到本地版本/内置版本（可配置）。

## 2. 版本/Manifest

- `<package>.version`：文本版本。
- `<package>_<version>.hash`：Manifest 内容 SHA-256（PRCM 160 字节头之后的 payload；与 `parcel_pack` 的 `manifest.hash` 一致）。
- `<package>_<version>.bytes`：PRCM。
- 先取 hash，再取 bytes；校验后解析。
- 本地缓存：`<persistent>/parcel/<package>/manifests/`。
- 下载采用临时文件 + 原子 rename。

## 3. Downloader

- 并发上限 + 每帧请求上限（16ms 调度窗口近似宿主帧；可配置）。
- Range 断点续传 + 最小续传尺寸；服务器不支持 Range 时重下。
- 重试策略：可重试错误、指数退避 + 抖动、最大次数。
- watchdog：长时间无进度中止。
- 暂停/恢复/取消；取消删除临时文件。
- 下载完成：size + SHA-256 校验 → 原子安装。
- 下载报告：总数/总字节/当前进度/失败列表/每文件事件。
- 失败列表支持 RetryFailed。
- 按需下载与显式 Downloader 共享按临时路径的 in-flight gate；同一 Bundle 只执行一次传输，等待方复用 owner 的结果。

## 4. CDN / 鉴权 / 灰度客户端

```cpp
class IRemoteConfigProvider {
    virtual Result<RemoteConfig> Load() = 0;
};

class IVersionResolver {
    virtual Result<VersionDecision> Resolve(const VersionContext&) = 0;
};

class IUrlProvider {
    virtual Result<std::vector<std::string>> GetUrls(const RequestContext&) = 0;
};

class SignedUrlProvider final : public IUrlProvider {
  public:
    SignedUrlProvider(std::string sign_url, std::vector<std::string> base_urls,
                      std::shared_ptr<runtime::IHttpTransport> transport,
                      std::shared_ptr<IAuthProvider> auth_provider = nullptr);
};

class IAuthProvider {
    virtual Result<std::vector<runtime::HttpHeader>>
    GetAuth(const RemoteRequestContext&) = 0;
    virtual Result<bool> OnUnauthorized(const RemoteRequestContext&) = 0;
};

auto url_provider = std::make_shared<SignedUrlProvider>(
    config.sign_url, config.cdn, transport, auth_provider);
```

能力：

- 多 CDN 候选、失败回退、健康标记。
- `SignedUrlProvider`：带 Token 请求 `/api/v1/client/url`，把签名 query 应用到 CDN 候选；缓存到过期，403/410 重新签名，客户端不持有 HMAC 密钥。
- Token/Header；401 刷新一次。
- 灰度：服务端返回版本优先；客户端按 `sha256(player_id + package + salt)` 分桶兜底。
- 白名单/百分比/地区/平台/App 版本。
- 回滚：保留旧版本 Manifest/Bundle；配置切回。
- 离线：缓存上次配置/版本决策；无网络使用本地版本。
- 一致性：一次会话版本固定；Manifest 原子切换；不混用两套 Manifest。

## 5. 配置 schema（客户端）

```json
{
  "schema": 1,
  "package": "game",
  "channel": "prod",
  "version": "1.0.0",
  "manifest_hash": "sha256:...",
  "cdn": [
    "https://cdn-a.example.com/game/",
    "https://cdn-b.example.com/game/"
  ],
  "sign_url": "https://api.example.com/api/v1/client/url",
  "auth": {
    "mode": "token+sign",
    "header": "Authorization",
    "token_url": "https://api.example.com/auth/device-token"
  },
  "gray": {
    "salt": "game-v1",
    "rules": [
      {
        "version": "1.1.0",
        "percentage": 10,
        "whitelist": ["player-001"],
        "platforms": ["windows"],
        "min_app_version": "1.0.0",
        "enabled": true
      }
    ]
  },
  "rollback": {
    "previous_version": "0.9.0"
  }
}
```

## 6. 安全边界

- 首期仅 SHA-256：防损坏，不防篡改；依赖 HTTPS + CDN 权限。
- 如果后续需要防篡改：增加 Manifest 签名 + Bundle 签名 + 公钥校验。
- 签名 URL 使用 HMAC-SHA256 + 过期时间 + key id；密钥轮换。
- 不记录 Token/签名/密钥。
- 下载路径禁止穿越；block/解压大小设上限。

## 7. 测试

- 本地 mock server：200/401/403/404/500、慢响应、断连、Range。
- 多 CDN 失败切换。
- Token 过期刷新；签名 URL 过期/403 重签（缓存命中与失效重签用例）。
- 灰度百分比/白名单/平台/App 版本；回滚。
- 断点续传、损坏文件修复、磁盘满、Manifest 损坏。
- watchdog：停滞传输在阈值后失败；持续收到字节时不会误触发。

## 8. R07 当前实现

### 8.1 模块

- `parcel_runtime`：
  - `IHttpTransport` / `IDownloadUrlProvider` / `DownloadOptions`。
  - watchdog：`HttpRequest::on_bytes_received` + `should_abort`，按每次传输进度重置；
    WinHTTP 在读取循环中检查 abort。
  - `ResourcePackage::CreateDownloader`：all/locations/tags 差异下载，排除 Builtin 已满足的 Bundle。
  - in-flight gate：按临时路径共享 owner/waiter，显式 Downloader 与按需下载复用同一传输。
  - `BundleDownloader`：并发 worker、每窗口请求配额、Range 续传、退避+抖动重试、暂停/恢复/取消、
    失败列表/RetryFailed、进度/事件、磁盘空间检查、SHA-256/CRC 校验后的原子安装。
    commit 失败（如磁盘满）会删除 `.part` 并按重试策略重新下载；达到最大次数后返回 `Io`，
    释放空间后重新创建 Downloader 可恢复。
  - `SandboxFileSystem`：`temp/<bundle-guid>.part` 临时文件、`CommitDownloadedBundle`、
    `ClearCache(OldVersions)` 保留当前 Manifest/已加载 Bundle。
  - `LoadAsset` / `LoadBundleFile` / `EnsureBundle` 缺失或损坏时可按需下载并重试。
- `parcel_remote`：
  - `ParseRemoteConfig` / `FileRemoteConfigProvider`。
  - `ManifestCache`：`<package>_<version>.bytes/.hash`、版本文本缓存、SHA-256 校验、
    原子写入、旧版本清理。
  - `RemoteManifestService`：RequestVersion、Manifest hash/bytes、缓存优先、
    原子 Active Manifest 切换、401/403 刷新一次、多 URL 失败切换。
  - `CdnUrlProvider`、`SignedUrlProvider`、`RemoteDownloadProvider`、`StaticAuthProvider`、
    `TokenAuthProvider`、`GrayVersionResolver`。
  - `WinHttpTransport`（Windows）：真实 HTTP/HTTPS、Range、超时和响应头解析；
    session/connection 句柄复用（`GetStats()` 记录 created/reused）；
    非 Windows 返回 `Unsupported`，宿主可注入其他 `IHttpTransport`。
- 测试使用进程内 `MockTransport` 覆盖 200/401/403/404/500、Range、慢响应、
  断连/传输失败、多 CDN、Token 刷新、Signed URL 缓存/403 重签、缓存修复、暂停/取消、每窗口请求配额、跨 Downloader 并发去重、
  超大/失败保留/孤儿临时文件、磁盘满 commit 注入、灰度/回滚/离线。
### 8.2 明确边界

- `max_requests_per_frame` 已接入调度：默认每 16ms 窗口最多启动 8 个请求（1-32），
  `scheduler_frame_interval<=0` 可关闭窗口限流；并发仍受 `max_concurrency` 限制。
- watchdog 为下载器级无进度计时器：`HttpRequest::on_bytes_received` 重置进度时钟，`should_abort` 让 transport 中断停滞读取；WinHTTP 同时保留接收超时兜底。
- 显式 Downloader 与按需下载共享按临时路径的 in-flight gate，同一 Bundle 等待 owner 完成；
  不再各自下载后再靠缓存复查。
  R07 不包含生产签名密钥。
- 自动测试使用 MockTransport；WinHTTP 真实网络 smoke 需宿主环境。















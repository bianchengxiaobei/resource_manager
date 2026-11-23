# 05 Runtime

## 1. 核心对象

```text
AssetSystem
  └─ ResourcePackage
       ├─ ResourceManager
       │    ├─ Provider（按 asset_id + load_method）
       │    └─ BundleLoader（按 BundleGuid）
       ├─ FileSystemHost
       │    ├─ BuiltinFileSystem
       │    ├─ SandboxFileSystem
       │    └─ CustomFileSystem
       ├─ Manifest（shared_ptr<const>）
       └─ Downloader / Remote
```

## 2. API 表面（全量对齐）

| 类别 | API |
|---|---|
| Package | Initialize / Destroy / RequestVersion / LoadManifest / PrefetchManifest / EnsureBundle |
| Asset | LoadAsset / LoadSubAssets / LoadAllAssets（Sync/Async，模板 + TypeId） |
| Raw/Bundle | LoadRawFile / LoadBundleFile |
| Scene | LoadScene / UnloadScene（依赖 `parcel_entt` + `ISceneLoader`） |
| Info | GetAllAssetInfos / ByTag / ByGuid / IsLocationValid / GetDownloadSize |
| Unload | UnloadAllAssets / UnloadUnusedAssets / TryUnloadUnusedAsset |
| Downloader | All / Tags / Locations / Pause / Resume / Cancel / RetryFailed / Events |
| Cache | ClearAll / ClearUnused / ByTags / ByLocations / Verify |
| Unpack/Import | ResourceUnpacker / ResourceImporter |
| Multi-package | Create / Get / Destroy / Priority |
| Custom | StartOperation(CustomAsyncOperation) |
| Editor/Web | 接口保留，返回 `Unsupported` |

- 同步 API 仅建议工具/编辑器使用；主线程同步等待有死锁风险。
- 不支持的 API 返回 `Unsupported` + 明确错误码，不删除接口。

## 3. Operation Scheduler

```cpp
enum class EStatus { None, Running, Succeeded, Failed, Canceled };

class OperationBase {
public:
    virtual void Start() = 0;
    virtual void Tick() = 0;          // 主线程
    virtual void Cancel();
    void AddChild(OperationPtr child);
    void OnComplete(CompletionFn cb);
    EStatus Status() const;
    float Progress() const;
    const Error& LastError() const;
};
```

- `AssetSystem::Update()` 驱动调度器；默认时间片 4ms，可配置。
- 优先级：Package 优先级 + Operation 优先级；防饿死。
- 子操作环检测；取消传播；abort/dispose 只执行一次。
- 完成回调帧末执行，允许回调中 Release/再次加载。
- 同步 `WaitForCompletion` 只给工具；它会主动驱动 Scheduler，不能与另一条正在驱动
  Scheduler 的主线程同时使用，否则可能死锁。

## 4. 生命周期

```text
Package:
  Uninitialized → Initializing → Ready
       ⇄ Updating / Downloading
       → Unloading (LoadingLock)
       → Destroyed

BundleLoader:
  None → WaitConcurrency → LoadingFile → Opening
       → Ready → Unloading → Destroyed

Provider:
  Created → WaitBundles → Deserialize → Done → Destroyed

Downloader:
  Created → Running ⇄ Paused → Completed / Failed / Canceled
```

规则：

- `UnloadAllAssets` / `DestroyPackage` 获取 LoadingLock，新加载返回失败句柄。
- Provider 加载中释放全部句柄：继续加载到完成后销毁，避免半完成。
- `UnloadUnusedAssets` 支持多轮依赖链卸载 + 时间片预算；预算 > 0 时按小批量 Provider/Bundle 卸载，
  单次调用以 4ms 为预算分批返回，未完成项由下一帧继续；预算 0 表示一次性全量清理。
  该增量语义对齐 YooAsset `AsyncOperationSystem.MaxTimeSlice` + `UnloadUnusedAssetsOperation` 分帧继续。
- `auto_unload_bundle_when_unused` 默认关闭；首期用显式 UnloadUnused。
- BundleLoader key 用 `BundleGuid`，热更期间新旧版本可共存。

## 5. 句柄与引用计数

```text
HandleState (shared_ptr，句柄拷贝共享)
   └─ shared_ptr<Provider>
Provider
   ├─ vector<shared_ptr<BundleLoader>>
   └─ vector<weak_ptr<HandleState>>
BundleLoader
   ├─ unique_ptr<IBundleHandle>
   ├─ provider_count
   └─ vector<weak_ptr<Provider>>
ResourceManager
   ├─ unordered_map<string, weak_ptr<Provider>>
   └─ unordered_map<string, weak_ptr<BundleLoader>>
```

- 最后一个 Handle 拷贝析构或 `Release()` 时释放 Provider。
- Provider 引用为 0 后移除；释放 BundleLoader 强引用。
- `AssetHandle::Get<T>()` 返回借用指针，句柄存活期间有效。
- Debug 下登记 Handle/Provider/Loader，退出报告泄漏。

## 6. FileSystem 与 Cache

- BuiltinFS：安装目录，只读。启动时读取 `builtin_catalog.bin`，按 Bundle
  FileName/`bundles/` 定位；catalog 包名必须与 Package 名一致。无 catalog 的旧包
  回退目录探测；catalog 未列出的 Bundle 视为非内置。
- SandboxFS：可写缓存；`CanAccept` 兜底 true；Host 模式顺序 `[Builtin, Sandbox]`。
- CustomFS：DLC/MOD/自定义挂载。
- 文件校验：size / CRC32 / SHA-256；下载必须 SHA-256。
- 缓存 sidecar：magic/version/GUID/size/SHA-256/CRC/flags。
- 写入：`.tmp` + 校验 + 原子 rename；失败删除临时文件。
- 启动只校验 size；加载失败或显式 Verify 才做 SHA/CRC。
- 损坏文件删除并重下；BuiltinCatalog 避免全目录扫描并支持下载差异准确排除内置 Bundle。
- App footprint/watermark：`FileSystemOptions::app_build_id` 非空时写入
  `ApplicationFootprint.bytes`；build id 变化时按 `InstallCleanupMode`
  （None/Bundles/Manifests/All）清理后再写入新水印，发生在 Manifest 加载前。
- 缓存清理：全部、未使用、按 tags、按 locations、旧版本。

## 7. 资源加载流程

```text
LoadAsset(location)
  → Manifest 解析 address/path/guid → AssetRecord
  → Provider（去重）
  → 主 Bundle + 依赖 Bundle（并发限制）
  → BundleReader 解析 PRCA
  → 两阶段反序列化（版本化 metadata + BlobTable）
  → IObjectResolver 解析跨资产引用
  → 创建 Handle，完成回调
```

- 依赖 Bundle 必须先加载；不允许隐式阻塞加载。
- 子资源/全部资源从同一 ObjectTable 派生。
- 跨 Bundle 引用通过 ExternalRefTable + `IObjectResolver`。
- 同一 Bundle/资产并发加载去重；失败错误关联到句柄。

## 8. 错误与诊断

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    std::string package;
    std::string bundle;
    std::string asset;
    std::string path;
    int httpStatus;
};
```

- 所有错误带上下文；不抛异常。
- 统计：Package/BundleLoader/Provider/Handle/Cache/Download。
- Debug：未释放句柄、Provider、BundleLoader、引用链。
- `OperationBase::Elapsed()` 返回操作开始到当前/完成的耗时；完成后冻结。
- `DownloadSummary::elapsed` 与 `bytes_per_second` 提供下载耗时和吞吐；
  `RuntimeStats::cache_bytes` 提供缓存占用。
- 错误码稳定，供服务端/日志/UI 使用。

## 9. 测试

- Handle/Provider/BundleLoader 随机释放、依赖链、泄漏。
- LoadingLock、UnloadAll、DestroyPackage、回调重入。
- Manifest 热切换、旧操作继续用旧 Manifest。
- 缓存损坏修复、断点续传、下载重试。
- 目标：1000 资源随机释放无泄漏；ASan/TSan（可行时）。

## 10. R05 实现说明

实现位于 `cpp/include/parcel/runtime/` 与 `cpp/src/runtime/`，target 为
`parcel_runtime`（namespace `parcel::runtime`）。核心文件：

```text
asset_system.cpp       AssetSystem、OperationScheduler、多 Package
resource_package.cpp   ResourcePackage、Provider、PRCA 多对象解码、卸载
bundle_loader.cpp      BundleLoader、BundleHandle、BlockTable/TOC
file_system.cpp        Builtin/Sandbox/Custom FS、Host、cache sidecar
handle.cpp             AssetHandle/HandleState
```

最小使用方式：

```cpp
parcel::runtime::AssetSystem system;
parcel::runtime::AssetSystemOptions system_options;
system_options.worker_threads = 4;
system.Initialize(system_options);

parcel::runtime::ResourcePackageOptions options;
options.package_name = "game";
options.builtin_root = "build/windows/game/1.0.0";
auto package = system.CreatePackage(options);

auto handle = package.Value()->LoadAsset<parcel::assets::MaterialAsset>(
    "a/<guid>.prca");
while (!handle.IsDone()) {
    system.Update();
}
auto* material = handle.Get<parcel::assets::MaterialAsset>();
handle.Release();
```

- 同步 `LoadAsset`/`LoadRawFile` 直接完成；异步 `Load*Async` 由
  `AssetSystem::Update()`（默认 4ms）驱动；`AssetHandle::WaitForCompletion()`
  只用于工具/测试，不能在 Scheduler 更新回调中调用。
- Provider key 为 `asset_guid + load_method + requested_type_id`；BundleLoader
  按 `BundleGuid` 去重。Provider 按主 Bundle + asset/bundle 依赖递归加载，
  `IObjectResolver` 从已加载 Bundle 解码 ExternalRefTable 引用。
- Handle 拷贝共享 `HandleState`；`Release()` 幂等。最后一个 Handle 释放
  Provider 引用；`auto_unload_bundle_when_unused` 默认关闭。
- `UnloadAllAssets` / `DestroyPackage` 会释放现有 Handle（之后 `IsValid()` 为
  false）并等待在途 Provider 完成；新加载在 LoadingLock 期间返回失败。
- `parcel_pack` 的 Bundle 位于版本目录的 `bundles/` 子目录；Builtin/Sandbox FS
  会先查找 `<root>/<file_name>`，再查找 `<root>/bundles/<file_name>`。
- Sandbox sidecar 为 `<bundle>.cache`，包含 magic/version/GUID/size/SHA-256/CRC；
  `LoadBundle` 失败会删除损坏文件和 sidecar。`manifest.hash` 仍作为 manifest
  内容 hash 校验。
- 已覆盖同步/异步、主/依赖 Bundle、Raw/BundleFile、全量/子资源、LoadingLock、
  UnloadAll、DestroyPackage、并发去重、回调作用域、缓存损坏、泄漏统计和
  多 Package API。当前测试为 `test_runtime.cpp`。
- `LoadScene`/`LoadSceneAsync` 在 Provider 集成阶段调用 `ISceneLoader::Instantiate`，
  registry 由 `SetSceneLoader(loader, registry)` 指定；异步场景在 Scheduler
  `Update` 时实例化。registry 必须比 SceneHandle 生命周期更长，销毁 registry 前
  应先 `UnloadScene` 或销毁 Package。
- `UnloadScene` 释放 SceneHandle；最后一个共享句柄释放时调用
  `ISceneLoader::Destroy`，随后释放 Provider 的依赖 Bundle。加载失败会清空
  Scene 对象并回滚 Bundle 引用。
- R05 边界：覆盖安装 footprint 与
  `OperationBase::Elapsed`/`DownloadSummary` 计时已实现；
  Unpacker/Importer 保留接口并返回 `Unsupported`。



## 12. R07 下载与缓存

- `ResourcePackageOptions` 可配置 `download_transport`、`download_url_provider`、
  `download_file_system` 和 `DownloadOptions`。
- `ResourcePackage::LoadManifest(ManifestPtr)`/`LoadManifestFromBytes` 可把远程 Manifest 原子应用到 Runtime。
- `ResourcePackage::CreateDownloader(locations, tags)` 按当前 Manifest 计算差异，
  排除 Builtin 已有 Bundle；同一 Bundle 只进入一次下载列表。
- `SandboxFileSystem` 使用 `temp/<bundle-guid>.part` 保存临时文件；
  下载成功后校验 size/SHA-256/CRC32，再 `rename` 到 `bundles/<file_name>` 并写 sidecar。
- 同一 Bundle 的并发下载按临时路径串行化；获得锁后先复查缓存，已提交则直接复用，
  避免多个 Downloader/按需加载同时写同一个 `.part`。
- `LoadAsset` / `LoadBundleFile` / `EnsureBundle` 在 Bundle 缺失或缓存损坏时，
  若配置了下载器则按需下载并重试一次。
- `ClearCache(OldVersions)` 保留当前 Manifest 和已加载 Bundle；
  `ClearCache(Unused)` 保留有引用计数 Bundle；`All` 清理 Bundle、sidecar 和 `.part`。
- `VerifyCacheFiles(level)` 对当前 Manifest 的每个 Bundle 做批量校验：`Metadata` 校验 sidecar/大小，`Size` 再核对磁盘大小，`Content`（默认）读取数据并核对 SHA-256/CRC32；缺失 Bundle 跳过，损坏项删除并继续扫描，返回 valid/missing/invalid/removed 统计。 `CacheVerifyOptions.max_concurrency` 控制并行读取，默认 8、范围 1-32（0 视为 1），实际 worker 数还受 CPU*2+1 限制；校验并行执行，删除/统计在全部 worker 结束后串行执行。
  并发语义对齐 YooAsset `FileVerifyMaxConcurrency` / `VerifyCacheFilesOperation`；`max_concurrency=1` 保持串行。
- `VerifyCache()` 使用 `Content` 级别批量校验；存在无法删除的损坏项时返回错误。
  行为对齐 YooAsset 初始化时的缓存搜索/校验/删除语义，但 Parcel 按 Manifest Bundle 枚举，不扫描无关孤儿文件。
- `QueryCache()` 返回缓存文件数/字节数。
- `Summary()` 额外返回 `elapsed` 和 `bytes_per_second`；运行中实时计算，完成后冻结。
- watchdog：`HttpRequest::on_bytes_received` 刷新进度时钟，`should_abort` 让 transport 中止无进度读取；WinHTTP 另用接收超时兜底。
- `max_requests_per_frame` 默认 8（1-32），按 `scheduler_frame_interval`（默认 16ms）窗口限流；`scheduler_frame_interval<=0` 关闭窗口限流，并发仍受 `max_concurrency` 限制。











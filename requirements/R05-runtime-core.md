# R05 Runtime 核心

状态：完成（2026-11-22）。已实现 `parcel_runtime`、Builtin/Sandbox/Custom
FileSystem、BundleLoader/Handle/Provider、PRCA 多对象/子资源、Resolver、同步/异步
加载、LoadingLock、UnloadAll、缓存 sidecar、诊断/泄漏报告和测试。
Scene 实例化/卸载已接入 R06 `ISceneLoader`；BuiltinCatalog 已加载并支持无 catalog 回退；
下载修复/断点续传依赖 R07；覆盖安装 footprint 已实现。
当前测试：206 cases / 5803 assertions；Debug/Release/no-exceptions/ASan/
exceptions/no-assimp 构建通过。

## 目标

实现 Parcel 运行时：AssetSystem/ResourcePackage/ResourceManager/Provider/Handle/BundleLoader/
FileSystem/Cache/BundleReader，覆盖全量 API 表面和生命周期。

## 前置依赖

R01、R02。

## 范围

- `parcel_runtime` 库。
- Builtin/Sandbox/Custom FileSystem。
- Bundle cache + sidecar + 校验/清理。
- Provider/Handle/BundleLoader 引用计数。
- 全量 API 表面（Scene 依赖 R06）。
- 错误/诊断/泄漏报告。

## 任务清单

### AssetSystem/Package

- [x] `AssetSystem`：Initialize/Shutdown/Update/CreatePackage/GetPackage/DestroyPackage。
- [x] `ResourcePackage`：Initialize/Destroy/Version/Manifest/Prefetch/Ensure。
- [x] 多 Package、优先级、独立 Manifest/Cache。
- [x] LoadingLock；UnloadAllAssets/DestroyPackage 等待在途操作。
- [x] 每帧 `Update` 驱动 OperationScheduler，默认 4ms 预算。

### FileSystem

- [x] `IFileSystem` 接口（Initialize/Version/Manifest/LoadBundle/Ensure/Download/ClearCache/
      CanAccept/IsDownloadRequired）。
- [x] BuiltinFS：只读；加载 `builtin_catalog.bin` 定位 Bundle，缺失/旧包回退目录探测。
- [x] SandboxFS：可写缓存、sidecar、清理、临时文件与 footprint。
- [x] CustomFS：用户挂载。
- [x] FileSystemHost：顺序匹配、main FS、依赖查询。
- [x] 缓存校验：size/CRC/SHA-256、损坏删除；重下由 R07 Downloader 完成。
- [x] App footprint/watermark；覆盖安装清理模式（None/Bundles/Manifests/All）。

### Bundle

- [x] `BundleReader`：PRCL 解析、BlockTable、TOC、按块读取。
- [x] `IBundleHandle`：LoadAsset/LoadAll/LoadSubAssets/LoadRawFile/Unload。
- [x] PRCA 反序列化：FlatBuffers/BlobTable、两阶段、ExternalRefTable。
- [x] `IObjectResolver`：AssetGuid 引用解析；依赖 Bundle 已加载。
- [x] BundleLoader：并发限制、重试、加载失败验证/修复。
- [x] BundleLoader key = BundleGuid；Provider key = asset_id + load_method。

### Handles/Provider

- [x] `HandleState` 共享句柄；Release 幂等；Get<T> 借用指针。
- [x] Provider 状态机；主/依赖 BundleLoader 引用；加载后回调。
- [x] 同资源并发加载去重；失败传播到所有句柄。
- [x] 释放全部句柄：加载中继续完成再销毁。
- [x] Provider/Handle 泄漏登记与报告。

### 全量 API

- [x] LoadAsset/LoadSubAssets/LoadAllAssets（Sync/Async、模板 + TypeId）。
- [x] LoadRawFile/LoadBundleFile。
- [x] LoadScene/UnloadScene：经 R06 `ISceneLoader` 实例化/销毁；无 loader 返回 Unsupported。
- [x] GetAllAssetInfos/ByTag/ByGuid/IsLocationValid/GetDownloadSize。
- [x] UnloadAllAssets/UnloadUnusedAssets/TryUnloadUnusedAsset。
- [x] CreateDownloader/ClearCache/Unpacker/Importer（接口）。
- [x] CustomAsyncOperation/StartOperation。
- [x] 未实现能力返回 `Unsupported`。

### 卸载

- [x] UnloadUnusedAssets 多轮依赖链 + 时间片。
- [x] auto_unload 默认关闭；开启时按引用归零尝试卸载。
- [x] 场景卸载释放 SceneHandle/依赖（R06）。
- [x] DestroyPackage 幂等；取消下载/Operation。

### 诊断

- [x] 统计：Package/BundleLoader/Provider/Handle/Cache/Download。
- [x] 错误码/上下文（package/bundle/asset/path/http）。
- [x] 操作耗时/进度/下载速度（`OperationBase::Elapsed`、`DownloadSummary::elapsed/bytes_per_second`）。
- [x] Debug 泄漏报告（句柄/引用链）。

## 验收标准

- 1000 资源随机加载/释放无泄漏。
- 同步/异步结果一致。
- 同一资源并发加载只加载一次；失败传播正确。
- 缓存损坏可检测/修复；下载缺失 Bundle 可按需补齐。
- LoadingLock/UnloadAll/DestroyPackage/回调重入测试通过。
- Manifest 热切换时旧操作继续使用旧 Manifest。
- 全量 API 可编译、错误可预测。

## 估算

约 4-5 周（1-2 人）。



























